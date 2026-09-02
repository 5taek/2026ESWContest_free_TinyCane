import 'dart:async';

import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:firebase_messaging/firebase_messaging.dart';
import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:geocoding/geocoding.dart';
import 'package:latlong2/latlong.dart';

import '../services/alert_messaging_service.dart';

class HomePage extends StatefulWidget {
  const HomePage({
    super.key,
    required this.user,
    required this.messagingService,
    this.messagingError,
  });

  final User user;
  final AlertMessagingService messagingService;
  final Object? messagingError;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  String? _lastShownDangerEventId;
  void _showDangerPopupIfNeeded(List<_FallEventView> events) {
    for (final event in events) {
      final data = event.data;
      final state = data['state']?.toString() ?? data['severity']?.toString();
      final status = data['status']?.toString() ?? 'active';
      if (!_isDangerState(state) || status == 'acknowledged') {
        continue;
      }

      if (_lastShownDangerEventId == event.id) {
        return;
      }

      _lastShownDangerEventId = event.id;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) {
          _showDangerEventDialog(context, event, widget.user);
        }
      });
      return;
    }
  }

  @override
  Widget build(BuildContext context) {
    final eventsQuery = FirebaseFirestore.instance
        .collection('fall_events')
        .where('guardianUids', arrayContains: widget.user.uid)
        .orderBy('createdAt', descending: true)
        .limit(50);
    // Computed once per build() rather than inline in the nested
    // StreamBuilder's `stream:` field below -- .snapshots() returns a new
    // Stream instance each call, and calling it fresh every time the OUTER
    // StreamBuilder's builder re-runs made the inner StreamBuilder see a
    // "different" stream and resubscribe, causing a loading-spinner flash
    // and a redundant re-fetch on every unrelated change to the user doc.
    final eventsStream = eventsQuery.snapshots();
    final userDocStream = FirebaseFirestore.instance
        .collection('users')
        .doc(widget.user.uid)
        .snapshots();

    return Scaffold(
      appBar: AppBar(
        title: const Text('Tiny Vision Cane'),
        centerTitle: false,
        backgroundColor: Theme.of(context).colorScheme.surface,
        actions: [
          IconButton(
            tooltip: '로그아웃',
            icon: const Icon(Icons.logout_rounded),
            onPressed: () => FirebaseAuth.instance.signOut(),
          ),
        ],
      ),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              _AlertStatus(
                user: widget.user,
                messagingService: widget.messagingService,
                messagingError: widget.messagingError,
              ),
              const SizedBox(height: 12),
              _LinkedPatientPanel(user: widget.user),
              const SizedBox(height: 12),
              Expanded(
                child: StreamBuilder<DocumentSnapshot<Map<String, dynamic>>>(
                  stream: userDocStream,
                  builder: (context, userSnapshot) {
                    final userData =
                        userSnapshot.data?.data() ?? <String, dynamic>{};
                    return StreamBuilder<QuerySnapshot<Map<String, dynamic>>>(
                      stream: eventsStream,
                      builder: (context, snapshot) {
                    if (snapshot.hasError) {
                      return Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          const _LocationMapPanel(location: _defaultLocation),
                          const SizedBox(height: 16),
                          _RecentEventsHeader(user: widget.user),
                          const SizedBox(height: 8),
                          Expanded(
                            child: _MessagePanel(
                              icon: Icons.error_outline_rounded,
                              title: '이벤트를 불러오지 못했습니다.',
                              message: _eventLoadErrorMessage(snapshot.error),
                            ),
                          ),
                        ],
                      );
                    }

                    if (!snapshot.hasData) {
                      return Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          const _LocationMapPanel(
                            location: _defaultLocation,
                            isLoading: true,
                          ),
                          const SizedBox(height: 16),
                          _RecentEventsHeader(user: widget.user),
                          const SizedBox(height: 8),
                          const Expanded(
                            child: Center(child: CircularProgressIndicator()),
                          ),
                        ],
                      );
                    }

                    final events = [...snapshot.data!.docs]
                      ..sort((a, b) => _createdAtMillis(b.data())
                          .compareTo(_createdAtMillis(a.data())));
                    final visibleEvents = events
                        .map(_FallEventView.fromSnapshot)
                        .toList(growable: false);
                    _showDangerPopupIfNeeded(visibleEvents);
                    final fallbackLocation =
                        _latestLocationFromEvents(visibleEvents) ??
                            _defaultLocation;

                    return Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        _PollingLocationPanel(
                          deviceId: _primaryDeviceId(userData),
                          fallbackLocation: fallbackLocation,
                        ),
                        const SizedBox(height: 16),
                        _RecentEventsHeader(user: widget.user),
                        const SizedBox(height: 8),
                        Expanded(
                          child: ListView.separated(
                            itemCount: visibleEvents.length,
                            separatorBuilder: (_, _) =>
                                const SizedBox(height: 8),
                            itemBuilder: (context, index) {
                              return _EventTile(
                                event: visibleEvents[index],
                                user: widget.user,
                              );
                            },
                          ),
                        ),
                      ],
                    );
                      },
                    );
                  },
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _AlertStatus extends StatefulWidget {
  const _AlertStatus({
    required this.user,
    required this.messagingService,
    required this.messagingError,
  });

  final User user;
  final AlertMessagingService messagingService;
  final Object? messagingError;

  @override
  State<_AlertStatus> createState() => _AlertStatusState();
}

class _AlertStatusState extends State<_AlertStatus> {
  late Future<AuthorizationStatus> _permissionFuture;
  var _isRequesting = false;

  @override
  void initState() {
    super.initState();
    _permissionFuture = widget.messagingService.permissionStatus();
  }

  Future<void> _requestPermissionAgain() async {
    setState(() {
      _isRequesting = true;
    });
    try {
      await widget.messagingService.requestPermissionAgain();
    } finally {
      if (mounted) {
        setState(() {
          _isRequesting = false;
          _permissionFuture = widget.messagingService.permissionStatus();
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return StreamBuilder<DocumentSnapshot<Map<String, dynamic>>>(
      stream: FirebaseFirestore.instance
          .collection('users')
          .doc(widget.user.uid)
          .snapshots(),
      builder: (context, snapshot) {
        final guardianId = snapshot.data?.data()?['guardianId']?.toString();
        final displayId =
            guardianId ?? _loginIdFromEmail(widget.user.email) ?? widget.user.uid;

        return FutureBuilder<AuthorizationStatus>(
          future: _permissionFuture,
          builder: (context, permissionSnapshot) {
            // A denied OS-level permission means push notifications will
            // never arrive no matter how healthy the token registration
            // looks -- this must not be conflated with the normal "waiting
            // for the first alert" state.
            final permissionDenied =
                permissionSnapshot.data == AuthorizationStatus.denied;

            final hasError = widget.messagingError != null;
            final title = permissionDenied
                ? '알림이 꺼져 있습니다'
                : hasError
                    ? '알림 등록 확인 필요'
                    : '보호자 알림 대기 중';
            final isProblem = permissionDenied || hasError;

            return Card(
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Row(
                      children: [
                        Container(
                          width: 44,
                          height: 44,
                          decoration: BoxDecoration(
                            color: isProblem
                                ? Theme.of(context).colorScheme.errorContainer
                                : Theme.of(context)
                                    .colorScheme
                                    .primaryContainer,
                            shape: BoxShape.circle,
                          ),
                          child: Icon(
                            isProblem
                                ? Icons.notifications_off_rounded
                                : Icons.notifications_active_rounded,
                            color: isProblem
                                ? Theme.of(context)
                                    .colorScheme
                                    .onErrorContainer
                                : Theme.of(context)
                                    .colorScheme
                                    .onPrimaryContainer,
                          ),
                        ),
                        const SizedBox(width: 14),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(
                                title,
                                style: Theme.of(context).textTheme.titleMedium,
                              ),
                              const SizedBox(height: 2),
                              Text(
                                '보호자 ID: $displayId',
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                              ),
                              if (permissionDenied) ...[
                                const SizedBox(height: 4),
                                Text(
                                  '기기 알림 권한이 꺼져 있어 낙상 알림을 받을 수 없습니다. '
                                  '설정 > 앱 > Tiny Vision Cane-보호자용 > '
                                  '알림에서 켜주세요.',
                                  style: TextStyle(
                                    color: Theme.of(context).colorScheme.error,
                                  ),
                                ),
                              ] else if (hasError) ...[
                                const SizedBox(height: 4),
                                Text(
                                  widget.messagingError.toString(),
                                  maxLines: 2,
                                  overflow: TextOverflow.ellipsis,
                                  style: TextStyle(
                                    color: Theme.of(context).colorScheme.error,
                                  ),
                                ),
                              ],
                            ],
                          ),
                        ),
                        FutureBuilder<String?>(
                          future: widget.messagingService.deviceToken(),
                          builder: (context, tokenSnapshot) {
                            final connected = !isProblem &&
                                tokenSnapshot.connectionState ==
                                    ConnectionState.done &&
                                tokenSnapshot.data != null;
                            return Icon(
                              connected
                                  ? Icons.cloud_done_rounded
                                  : Icons.cloud_queue,
                              color: connected
                                  ? Theme.of(context).colorScheme.primary
                                  : Theme.of(context).colorScheme.outline,
                            );
                          },
                        ),
                      ],
                    ),
                    if (permissionDenied) ...[
                      const SizedBox(height: 10),
                      Align(
                        alignment: Alignment.centerRight,
                        child: OutlinedButton.icon(
                          onPressed:
                              _isRequesting ? null : _requestPermissionAgain,
                          icon: _isRequesting
                              ? const SizedBox(
                                  width: 16,
                                  height: 16,
                                  child: CircularProgressIndicator(
                                    strokeWidth: 2,
                                  ),
                                )
                              : const Icon(Icons.notifications_rounded,
                                  size: 18),
                          label: const Text('알림 다시 요청'),
                        ),
                      ),
                    ],
                  ],
                ),
              ),
            );
          },
        );
      },
    );
  }
}

class _LinkedPatientPanel extends StatelessWidget {
  const _LinkedPatientPanel({required this.user});

  final User user;

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Row(
              children: [
                Icon(
                  Icons.sensors_rounded,
                  color: Theme.of(context).colorScheme.primary,
                ),
                const SizedBox(width: 8),
                Text(
                  '연결된 사용자',
                  style: Theme.of(context).textTheme.titleMedium,
                ),
              ],
            ),
            const SizedBox(height: 10),
            StreamBuilder<DocumentSnapshot<Map<String, dynamic>>>(
              stream: FirebaseFirestore.instance
                  .collection('users')
                  .doc(user.uid)
                  .snapshots(),
              builder: (context, snapshot) {
                if (!snapshot.hasData) {
                  return const LinearProgressIndicator();
                }

                final data = snapshot.data!.data() ?? <String, dynamic>{};
                final patientIds = _stringList(data['patientIds']);
                final deviceIds = _stringList(data['deviceIds']);
                if (patientIds.isEmpty && deviceIds.isEmpty) {
                  return const Text('연결된 사용자 정보가 없습니다.');
                }

                return Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    for (final patientId in patientIds)
                      _InfoChip(
                        icon: Icons.accessibility_new_rounded,
                        label: '사용자 $patientId',
                      ),
                    for (final deviceId in deviceIds)
                      _InfoChip(
                        icon: Icons.sensors_rounded,
                        label: '기기 $deviceId',
                      ),
                  ],
                );
              },
            ),
          ],
        ),
      ),
    );
  }
}

class _InfoChip extends StatelessWidget {
  const _InfoChip({
    required this.icon,
    required this.label,
  });

  final IconData icon;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Chip(
      avatar: Icon(icon, size: 18),
      label: Text(label),
    );
  }
}

class _RecentEventsHeader extends StatelessWidget {
  const _RecentEventsHeader({required this.user});

  final User user;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(
          child: Text(
            '최근 감지 기록',
            style: Theme.of(context).textTheme.titleMedium,
          ),
        ),
        OutlinedButton.icon(
          onPressed: () {
            Navigator.of(context).push(
              MaterialPageRoute<void>(
                builder: (_) => _EventLogPage(user: user),
              ),
            );
          },
          icon: const Icon(Icons.history_rounded, size: 18),
          label: const Text('로그'),
          style: OutlinedButton.styleFrom(
            visualDensity: VisualDensity.compact,
            minimumSize: const Size(0, 36),
            padding: const EdgeInsets.symmetric(horizontal: 12),
          ),
        ),
      ],
    );
  }
}

class _EventLogPage extends StatelessWidget {
  const _EventLogPage({required this.user});

  final User user;

  @override
  Widget build(BuildContext context) {
    final logsQuery = FirebaseFirestore.instance
        .collection('fall_events')
        .where('guardianUids', arrayContains: user.uid)
        .orderBy('createdAt', descending: true)
        .limit(200);

    return Scaffold(
      appBar: AppBar(
        title: const Text('감지 로그'),
        backgroundColor: Theme.of(context).colorScheme.surface,
      ),
      body: SafeArea(
        child: StreamBuilder<QuerySnapshot<Map<String, dynamic>>>(
          stream: logsQuery.snapshots(),
          builder: (context, snapshot) {
            if (snapshot.hasError) {
              return _MessagePanel(
                icon: Icons.error_outline_rounded,
                title: '로그를 불러오지 못했습니다.',
                message: snapshot.error.toString(),
              );
            }

            if (!snapshot.hasData) {
              return const Center(child: CircularProgressIndicator());
            }

            final events = [...snapshot.data!.docs]
              ..sort((a, b) => _createdAtMillis(b.data())
                  .compareTo(_createdAtMillis(a.data())));
            if (events.isEmpty) {
              return const _MessagePanel(
                icon: Icons.history_rounded,
                title: '감지 로그가 없습니다.',
                message: '기기에서 상태가 전송되면 여기에 누적됩니다.',
              );
            }

            final eventViews = events
                .map(_FallEventView.fromSnapshot)
                .toList(growable: false);

            return ListView.separated(
              padding: const EdgeInsets.all(16),
              itemCount: eventViews.length,
              separatorBuilder: (_, _) => const SizedBox(height: 8),
              itemBuilder: (context, index) {
                return _EventTile(
                  event: eventViews[index],
                  user: user,
                );
              },
            );
          },
        ),
      ),
    );
  }
}

class _EventLocation {
  const _EventLocation({
    required this.latitude,
    required this.longitude,
    this.address,
    this.createdAt,
  });

  final double latitude;
  final double longitude;
  final String? address;
  final Object? createdAt;
}

class _FallEventView {
  const _FallEventView({
    required this.id,
    required this.data,
    this.reference,
  });

  factory _FallEventView.fromSnapshot(
    QueryDocumentSnapshot<Map<String, dynamic>> snapshot,
  ) {
    return _FallEventView(
      id: snapshot.id,
      data: snapshot.data(),
      reference: snapshot.reference,
    );
  }

  final String id;
  final Map<String, dynamic> data;
  final DocumentReference<Map<String, dynamic>>? reference;
}

const _defaultLocation = _EventLocation(
  latitude: 37.5638209021699,
  longitude: 126.96237696604,
  createdAt: '기본 위치',
);
const _initialZoom = 16.0;
const _locationPollInterval = Duration(seconds: 10);

class _PollingLocationPanel extends StatefulWidget {
  const _PollingLocationPanel({
    required this.deviceId,
    required this.fallbackLocation,
  });

  final String deviceId;
  final _EventLocation fallbackLocation;

  @override
  State<_PollingLocationPanel> createState() => _PollingLocationPanelState();
}

class _PollingLocationPanelState extends State<_PollingLocationPanel> {
  Timer? _timer;
  _EventLocation? _location;
  var _isLoading = true;

  @override
  void initState() {
    super.initState();
    _startPolling();
  }

  @override
  void didUpdateWidget(covariant _PollingLocationPanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.deviceId != widget.deviceId) {
      _location = null;
      _isLoading = true;
      _startPolling();
    }
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  void _startPolling() {
    _timer?.cancel();
    _fetchLocation();
    _timer = Timer.periodic(_locationPollInterval, (_) {
      _fetchLocation();
    });
  }

  Future<void> _fetchLocation() async {
    if (!mounted) {
      return;
    }

    try {
      final snapshot = await FirebaseFirestore.instance
          .collection('devices')
          .doc(widget.deviceId)
          .get();
      final data = snapshot.data();
      final nextLocation =
          data == null ? null : _locationFromDeviceData(data);

      if (!mounted) {
        return;
      }

      setState(() {
        if (nextLocation != null) {
          _location = nextLocation;
        }
        _isLoading = false;
      });
    } catch (_) {
      if (!mounted) {
        return;
      }

      setState(() {
        _isLoading = false;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return _LocationMapPanel(
      location: _location ?? widget.fallbackLocation,
      isLoading: _isLoading,
    );
  }
}

class _LocationMapPanel extends StatelessWidget {
  const _LocationMapPanel({
    this.location,
    this.isLoading = false,
  });

  final _EventLocation? location;
  final bool isLoading;

  @override
  Widget build(BuildContext context) {
    final location = this.location;

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Row(
              children: [
                Icon(
                  Icons.location_on_rounded,
                  color: Theme.of(context).colorScheme.primary,
                ),
                const SizedBox(width: 8),
                Text(
                  '사용자 위치',
                  style: Theme.of(context).textTheme.titleMedium,
                ),
              ],
            ),
            const SizedBox(height: 10),
            SizedBox(
              height: 190,
              child: location == null
                  ? _MapPlaceholder(isLoading: isLoading)
                  : _EventMap(location: location),
            ),
            const SizedBox(height: 10),
            if (location == null)
              Text(
                isLoading ? '위치 정보를 불러오는 중입니다.' : '아직 수신된 위치 정보가 없습니다.',
                style: Theme.of(context).textTheme.bodyMedium,
              )
            else
              _LocationSummary(location: location),
          ],
        ),
      ),
    );
  }
}

class _EventMap extends StatefulWidget {
  const _EventMap({required this.location});

  final _EventLocation location;

  @override
  State<_EventMap> createState() => _EventMapState();
}

class _EventMapState extends State<_EventMap> {
  static const _minZoom = 4.0;
  static const _maxZoom = 18.0;

  final _mapController = MapController();
  late var _zoom = _initialZoom;

  LatLng get _point {
    return LatLng(widget.location.latitude, widget.location.longitude);
  }

  @override
  void dispose() {
    // FlutterMap only auto-disposes a MapController it created internally
    // (when no mapController is passed in). Since one is supplied here
    // explicitly, it's never released on its own -- every time this map
    // view is torn down (navigate away, logout/login, location becomes
    // unavailable) leaked the previous controller.
    _mapController.dispose();
    super.dispose();
  }

  @override
  void didUpdateWidget(covariant _EventMap oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.location.latitude != widget.location.latitude ||
        oldWidget.location.longitude != widget.location.longitude) {
      _zoom = _initialZoom;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) {
          _mapController.move(_point, _zoom);
        }
      });
    }
  }

  void _setZoom(double nextZoom) {
    final clampedZoom = nextZoom.clamp(_minZoom, _maxZoom).toDouble();
    setState(() {
      _zoom = clampedZoom;
    });
    _mapController.move(_point, clampedZoom);
  }

  void _resetView() {
    _setZoom(_initialZoom);
  }

  @override
  Widget build(BuildContext context) {
    final point = _point;

    return ClipRRect(
      borderRadius: BorderRadius.circular(8),
      child: Stack(
        children: [
          const _MapBackdrop(),
          FlutterMap(
            mapController: _mapController,
            key: ValueKey(
              '${widget.location.latitude},${widget.location.longitude}',
            ),
            options: MapOptions(
              initialCenter: point,
              initialZoom: _initialZoom,
              minZoom: _minZoom,
              maxZoom: _maxZoom,
              interactionOptions: const InteractionOptions(
                flags: InteractiveFlag.all,
              ),
              onPositionChanged: (camera, hasGesture) {
                if (hasGesture && camera.zoom != _zoom) {
                  setState(() {
                    _zoom = camera.zoom;
                  });
                }
              },
            ),
            children: [
              TileLayer(
                urlTemplate: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
                fallbackUrl:
                    'https://a.basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png',
                userAgentPackageName: 'com.tinyvisioncane.guardian',
                tileBuilder: (context, tileWidget, tile) {
                  if (tile.loadError) {
                    return const _MapTileError();
                  }

                  return tileWidget;
                },
              ),
              MarkerLayer(
                markers: [
                  Marker(
                    point: point,
                    width: 44,
                    height: 44,
                    child: Icon(
                      Icons.location_on_rounded,
                      size: 42,
                      color: Theme.of(context).colorScheme.error,
                    ),
                  ),
                ],
              ),
            ],
          ),
          Positioned(
            right: 6,
            bottom: 6,
            child: DecoratedBox(
              decoration: BoxDecoration(
                color: Colors.white.withValues(alpha: 0.86),
                borderRadius: BorderRadius.circular(4),
              ),
              child: const Padding(
                padding: EdgeInsets.symmetric(horizontal: 6, vertical: 3),
                child: Text(
              '© OpenStreetMap',
                  style: TextStyle(fontSize: 10),
                ),
              ),
            ),
          ),
          Positioned(
            right: 8,
            top: 8,
            child: _MapZoomControls(
              canZoomIn: _zoom < _maxZoom,
              canZoomOut: _zoom > _minZoom,
              onZoomIn: () => _setZoom(_zoom + 1),
              onZoomOut: () => _setZoom(_zoom - 1),
              onReset: _resetView,
            ),
          ),
        ],
      ),
    );
  }
}

class _MapZoomControls extends StatelessWidget {
  const _MapZoomControls({
    required this.canZoomIn,
    required this.canZoomOut,
    required this.onZoomIn,
    required this.onZoomOut,
    required this.onReset,
  });

  final bool canZoomIn;
  final bool canZoomOut;
  final VoidCallback onZoomIn;
  final VoidCallback onZoomOut;
  final VoidCallback onReset;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.92),
        borderRadius: BorderRadius.circular(8),
        boxShadow: const [
          BoxShadow(
            color: Color(0x22000000),
            blurRadius: 8,
            offset: Offset(0, 2),
          ),
        ],
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          _MapControlButton(
            tooltip: '지도 확대',
            icon: Icons.add_rounded,
            onPressed: canZoomIn ? onZoomIn : null,
          ),
          _MapControlDivider(),
          _MapControlButton(
            tooltip: '지도 축소',
            icon: Icons.remove_rounded,
            onPressed: canZoomOut ? onZoomOut : null,
          ),
          _MapControlDivider(),
          _MapControlButton(
            tooltip: '위치로 이동',
            icon: Icons.my_location_rounded,
            onPressed: onReset,
          ),
        ],
      ),
    );
  }
}

class _MapControlButton extends StatelessWidget {
  const _MapControlButton({
    required this.tooltip,
    required this.icon,
    required this.onPressed,
  });

  final String tooltip;
  final IconData icon;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    return IconButton(
      tooltip: tooltip,
      visualDensity: VisualDensity.compact,
      iconSize: 20,
      constraints: const BoxConstraints.tightFor(width: 36, height: 34),
      onPressed: onPressed,
      icon: Icon(icon),
    );
  }
}

class _MapControlDivider extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 28,
      child: Divider(
        height: 1,
        color: Theme.of(context).colorScheme.outlineVariant,
      ),
    );
  }
}

class _MapBackdrop extends StatelessWidget {
  const _MapBackdrop();

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surfaceContainerHighest,
      ),
      child: CustomPaint(
        painter: _GridPainter(
          color: Theme.of(context).colorScheme.outlineVariant,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}

class _MapTileError extends StatelessWidget {
  const _MapTileError();

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surfaceContainerHighest,
        border: Border.all(
          color: Theme.of(context).colorScheme.outlineVariant,
          width: 0.5,
        ),
      ),
      child: Center(
        child: Icon(
          Icons.wifi_off_rounded,
          size: 18,
          color: Theme.of(context).colorScheme.outline,
        ),
      ),
    );
  }
}

class _GridPainter extends CustomPainter {
  const _GridPainter({required this.color});

  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = color
      ..strokeWidth = 0.6;
    const step = 28.0;

    for (double x = 0; x <= size.width; x += step) {
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), paint);
    }
    for (double y = 0; y <= size.height; y += step) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), paint);
    }
  }

  @override
  bool shouldRepaint(_GridPainter oldDelegate) {
    return oldDelegate.color != color;
  }
}

class _MapPlaceholder extends StatelessWidget {
  const _MapPlaceholder({required this.isLoading});

  final bool isLoading;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surfaceContainerHighest,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Center(
        child: isLoading
            ? const CircularProgressIndicator()
            : Icon(
                Icons.map_outlined,
                size: 44,
                color: Theme.of(context).colorScheme.outline,
              ),
      ),
    );
  }
}

class _LocationSummary extends StatelessWidget {
  const _LocationSummary({required this.location});

  final _EventLocation location;

  @override
  Widget build(BuildContext context) {
    return FutureBuilder<String>(
      future: _addressForLocation(location),
      builder: (context, snapshot) {
        final address = snapshot.data ?? location.address ?? '주소를 확인하는 중입니다.';

        return Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              address,
              maxLines: 2,
              overflow: TextOverflow.ellipsis,
              style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                    fontWeight: FontWeight.w700,
                  ),
            ),
            const SizedBox(height: 3),
            Text(
              '${location.latitude.toStringAsFixed(6)}, '
              '${location.longitude.toStringAsFixed(6)}',
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: Theme.of(context).textTheme.bodySmall,
            ),
            const SizedBox(height: 3),
            Text(
              '업데이트: ${_formatLocationUpdatedAt(location.createdAt)}',
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ],
        );
      },
    );
  }
}

class _EventTile extends StatelessWidget {
  const _EventTile({required this.event, required this.user});

  final _FallEventView event;
  final User user;

  @override
  Widget build(BuildContext context) {
    final data = event.data;
    final status = data['status']?.toString() ?? 'active';
    final isActive = status != 'acknowledged';
    final state = data['state']?.toString() ?? data['severity']?.toString();
    final stateInfo = _stateInfo(context, state);
    final patientId = data['patientId']?.toString() ?? '대상자 미지정';
    final deviceId = data['deviceId']?.toString() ?? '기기 미지정';
    final message = data['message']?.toString() ?? stateInfo.message;
    final locationText = _formatLocation(data['location']);

    return Card(
      child: ListTile(
        minVerticalPadding: 14,
        leading: CircleAvatar(
          backgroundColor: stateInfo.backgroundColor,
          child: Icon(stateInfo.icon, color: stateInfo.foregroundColor),
        ),
        title: Text(
          message,
          maxLines: 2,
          overflow: TextOverflow.ellipsis,
        ),
        subtitle: Padding(
          padding: const EdgeInsets.only(top: 6),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                '$patientId · ${stateInfo.label} · $deviceId · '
                '${_formatCreatedAt(data['createdAt'])}',
              ),
              if (locationText != null) ...[
                const SizedBox(height: 2),
                Text('위치: $locationText'),
              ],
            ],
          ),
        ),
        trailing: isActive
            ? IconButton(
                tooltip: '확인',
                icon: const Icon(Icons.check_circle_outline_rounded),
                onPressed: () => _acknowledge(context),
              )
            : const Text('확인됨'),
      ),
    );
  }

  Future<void> _acknowledge(BuildContext context) async {
    final reference = event.reference;
    if (reference == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('시연용 기록입니다. 실제 기록은 변경하지 않습니다.')),
      );
      return;
    }

    try {
      await reference.update({
        'status': 'acknowledged',
        'acknowledgedAt': FieldValue.serverTimestamp(),
        'acknowledgedBy': user.uid,
      });
    } catch (error) {
      if (!context.mounted) {
        return;
      }

      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('확인 처리 실패: $error')),
      );
    }
  }
}

Future<void> _showDangerEventDialog(
  BuildContext context,
  _FallEventView event,
  User user,
) async {
  final data = event.data;
  final stateInfo = _stateInfo(context, data['state']?.toString());
  final patientId = data['patientId']?.toString() ?? '대상자 미지정';
  final deviceId = data['deviceId']?.toString() ?? '기기 미지정';
  final message = data['message']?.toString() ?? stateInfo.message;
  final location = _locationFromEventData(data);
  final locationText = location == null
      ? _formatLocation(data['location']) ?? '위치 정보 없음'
      : '${location.latitude.toStringAsFixed(6)}, '
          '${location.longitude.toStringAsFixed(6)}';

  await showDialog<void>(
    context: context,
    barrierDismissible: true,
    builder: (dialogContext) {
      return AlertDialog(
        icon: Icon(
          Icons.warning_amber_rounded,
          color: Theme.of(dialogContext).colorScheme.error,
          size: 40,
        ),
        title: const Text('위험 상황 발생'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              message,
              style: Theme.of(dialogContext).textTheme.bodyLarge?.copyWith(
                    fontWeight: FontWeight.w700,
                  ),
            ),
            const SizedBox(height: 14),
            Text('사용자: $patientId'),
            Text('기기: $deviceId'),
            Text('발생 시간: ${_formatCreatedAt(data['createdAt'])}'),
            Text('위치: $locationText'),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(dialogContext).pop(),
            child: const Text('닫기'),
          ),
          FilledButton.icon(
            onPressed: () async {
              final reference = event.reference;
              if (reference == null) {
                Navigator.of(dialogContext).pop();
                return;
              }

              try {
                await reference.update({
                  'status': 'acknowledged',
                  'acknowledgedAt': FieldValue.serverTimestamp(),
                  'acknowledgedBy': user.uid,
                });
                if (dialogContext.mounted) {
                  Navigator.of(dialogContext).pop();
                }
              } catch (error) {
                if (!dialogContext.mounted) {
                  return;
                }
                ScaffoldMessenger.of(dialogContext).showSnackBar(
                  SnackBar(content: Text('확인 처리 실패: $error')),
                );
              }
            },
            icon: const Icon(Icons.check_circle_outline_rounded),
            label: const Text('확인 처리'),
          ),
        ],
      );
    },
  );
}

class _StateInfo {
  const _StateInfo({
    required this.label,
    required this.message,
    required this.icon,
    required this.backgroundColor,
    required this.foregroundColor,
  });

  final String label;
  final String message;
  final IconData icon;
  final Color backgroundColor;
  final Color foregroundColor;
}

bool _isDangerState(String? rawState) {
  return rawState == 'danger_fall' || rawState == 'danger';
}

_StateInfo _stateInfo(BuildContext context, String? rawState) {
  final state = rawState ?? 'danger_fall';
  final colorScheme = Theme.of(context).colorScheme;

  switch (state) {
    case 'normal':
      return _StateInfo(
        label: '안전',
        message: '사용자가 안전 상태입니다.',
        icon: Icons.check_rounded,
        backgroundColor: colorScheme.primaryContainer,
        foregroundColor: colorScheme.onPrimaryContainer,
      );
    case 'safe_fall':
    case 'safe':
      return _StateInfo(
        label: '주의',
        message: '낙상/지팡이 낙하가 감지되었습니다.',
        icon: Icons.report_problem_rounded,
        backgroundColor: colorScheme.secondaryContainer,
        foregroundColor: colorScheme.onSecondaryContainer,
      );
    case 'danger_fall':
    case 'danger':
    default:
      return _StateInfo(
        label: '위험 낙상',
        message: '사용자가 낙상 후 위험 상태로 판단됩니다. 보호자 확인이 필요합니다.',
        icon: Icons.warning_amber_rounded,
        backgroundColor: colorScheme.errorContainer,
        foregroundColor: colorScheme.onErrorContainer,
      );
  }
}

String _formatCreatedAt(Object? raw) {
  if (raw is Timestamp) {
    return _formatDateTime(raw.toDate().toLocal());
  }

  if (raw is DateTime) {
    return _formatDateTime(raw.toLocal());
  }

  if (raw is String && raw.isNotEmpty) {
    final parsed = DateTime.tryParse(raw);
    if (parsed != null) {
      return _formatDateTime(parsed.toLocal());
    }
    return raw;
  }

  return '시간 기록 전';
}

String _formatDateTime(DateTime dateTime) {
  return '${dateTime.year}-${_two(dateTime.month)}-${_two(dateTime.day)} '
      '${_two(dateTime.hour)}:${_two(dateTime.minute)}:${_two(dateTime.second)}';
}

String _eventLoadErrorMessage(Object? error) {
  if (error is FirebaseException && error.code == 'permission-denied') {
    return '이벤트 접근 권한이 없습니다. 잠시 후 다시 시도해 주세요.';
  }
  if (error is FirebaseException && error.code == 'failed-precondition') {
    return '이벤트 조회 설정이 아직 준비되지 않았습니다.';
  }
  return '네트워크 상태를 확인하고 다시 시도해 주세요.';
}

int _createdAtMillis(Map<String, dynamic> data) {
  final raw = data['createdAt'];
  if (raw is Timestamp) {
    return raw.millisecondsSinceEpoch;
  }
  if (raw is DateTime) {
    return raw.millisecondsSinceEpoch;
  }
  if (raw is String) {
    return DateTime.tryParse(raw)?.millisecondsSinceEpoch ?? 0;
  }
  return 0;
}

_EventLocation? _latestLocationFromEvents(
  List<_FallEventView> events,
) {
  for (final event in events) {
    final location = _locationFromEventData(event.data);
    if (location != null) {
      return location;
    }
  }

  return null;
}

_EventLocation? _locationFromEventData(Map<String, dynamic> data) {
  final rawLocation = data['location'];
  final locationMap = rawLocation is Map ? rawLocation : const {};
  final latitude = _toDouble(
    locationMap['latitude'] ?? locationMap['lat'] ?? data['latitude'] ?? data['lat'],
  );
  final longitude = _toDouble(
    locationMap['longitude'] ??
        locationMap['lng'] ??
        locationMap['lon'] ??
        data['longitude'] ??
        data['lng'] ??
        data['lon'],
  );

  if (latitude == null || longitude == null) {
    return null;
  }

  return _EventLocation(
    latitude: latitude,
    longitude: longitude,
    address: _firstText([
      data['address'],
      locationMap['address'],
      locationMap['formattedAddress'],
      locationMap['fullAddress'],
    ]),
    createdAt: data['createdAt'] ??
        locationMap['timestamp'] ??
        locationMap['updatedAt'],
  );
}

_EventLocation? _locationFromDeviceData(Map<String, dynamic> data) {
  final rawLocation = data['latestLocation'] ?? data['location'];
  final locationMap = rawLocation is Map ? rawLocation : const {};
  final latitude = _toDouble(
    locationMap['latitude'] ??
        locationMap['lat'] ??
        data['latitude'] ??
        data['lat'],
  );
  final longitude = _toDouble(
    locationMap['longitude'] ??
        locationMap['lng'] ??
        locationMap['lon'] ??
        data['longitude'] ??
        data['lng'] ??
        data['lon'],
  );

  if (latitude == null || longitude == null) {
    return null;
  }

  return _EventLocation(
    latitude: latitude,
    longitude: longitude,
    address: _firstText([
      locationMap['address'],
      locationMap['formattedAddress'],
      locationMap['fullAddress'],
      data['address'],
    ]),
    createdAt: locationMap['updatedAt'] ??
        locationMap['timestamp'] ??
        data['lastSeenAt'] ??
        data['updatedAt'],
  );
}

// Keyed by coordinates rounded to ~11m precision. _PollingLocationPanel
// rebuilds _LocationSummary every 10s, and polled device locations rarely
// carry a pre-resolved `address`, so without this cache the same spot was
// re-geocoded over the network on every single poll tick indefinitely.
final _addressCache = <String, Future<String>>{};

Future<String> _addressForLocation(_EventLocation location) {
  if (location.address != null && location.address!.isNotEmpty) {
    return Future.value(location.address);
  }

  final key =
      '${location.latitude.toStringAsFixed(4)},${location.longitude.toStringAsFixed(4)}';
  return _addressCache.putIfAbsent(
    key,
    () => _resolveAddress(location, key),
  );
}

Future<String> _resolveAddress(_EventLocation location, String cacheKey) async {
  try {
    final placemarks = await Geocoding().placemarkFromCoordinates(
      location.latitude,
      location.longitude,
      locale: const Locale('ko', 'KR'),
    );
    if (placemarks.isEmpty) {
      // Don't cache "no result" -- retry on the next poll instead of
      // getting stuck on a possibly-transient empty response forever.
      _addressCache.remove(cacheKey);
      return '주소를 찾을 수 없습니다.';
    }

    return _formatPlacemark(placemarks.first);
  } catch (_) {
    // Don't cache a network/lookup failure either -- same reasoning.
    _addressCache.remove(cacheKey);
    return '주소 확인 실패';
  }
}

String _formatPlacemark(Placemark placemark) {
  final parts = [
    placemark.administrativeArea,
    placemark.locality,
    placemark.subLocality,
    placemark.thoroughfare,
    placemark.subThoroughfare,
  ]
      .whereType<String>()
      .map((value) => value.trim())
      .where((value) => value.isNotEmpty)
      .toList(growable: false);

  if (parts.isNotEmpty) {
    return parts.toSet().join(' ');
  }

  final fallback = _firstText([
    placemark.street,
    placemark.name,
    placemark.country,
  ]);
  return fallback ?? '주소를 찾을 수 없습니다.';
}

String _formatLocationUpdatedAt(Object? raw) {
  if (raw == null) {
    return '수신 기록 없음';
  }

  return _formatCreatedAt(raw);
}

double? _toDouble(Object? raw) {
  if (raw is num) {
    return raw.toDouble();
  }

  if (raw is String) {
    return double.tryParse(raw);
  }

  return null;
}

String? _firstText(Iterable<Object?> values) {
  for (final value in values) {
    final text = value?.toString().trim();
    if (text != null && text.isNotEmpty) {
      return text;
    }
  }

  return null;
}

String? _formatLocation(Object? raw) {
  if (raw is! Map) {
    return null;
  }

  final latitude = raw['latitude'] ?? raw['lat'];
  final longitude = raw['longitude'] ?? raw['lng'] ?? raw['lon'];
  if (latitude == null || longitude == null) {
    return null;
  }

  return '$latitude, $longitude';
}

String _primaryDeviceId(Map<String, dynamic> userData) {
  final deviceIds = _stringList(userData['deviceIds']);
  if (deviceIds.isNotEmpty) {
    return deviceIds.first;
  }

  final deviceId = userData['deviceId']?.toString();
  if (deviceId != null && deviceId.isNotEmpty) {
    return deviceId;
  }

  return 'cane-001';
}

String? _loginIdFromEmail(String? email) {
  if (email == null) {
    return null;
  }

  final parts = email.split('@');
  if (parts.length != 2) {
    return email;
  }

  return parts.first;
}

List<String> _stringList(Object? raw) {
  if (raw is! Iterable) {
    return const [];
  }

  return raw
      .map((value) => value.toString())
      .where((value) => value.isNotEmpty)
      .toList(growable: false);
}

String _two(int value) => value.toString().padLeft(2, '0');

class _MessagePanel extends StatelessWidget {
  const _MessagePanel({
    required this.icon,
    required this.title,
    required this.message,
  });

  final IconData icon;
  final String title;
  final String message;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: SingleChildScrollView(
        child: Card(
          child: Padding(
            padding: const EdgeInsets.all(20),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(
                  icon,
                  size: 36,
                  color: Theme.of(context).colorScheme.primary,
                ),
                const SizedBox(height: 12),
                Text(
                  title,
                  textAlign: TextAlign.center,
                  style: Theme.of(context).textTheme.titleMedium,
                ),
                const SizedBox(height: 6),
                Text(
                  message,
                  textAlign: TextAlign.center,
                  style: Theme.of(context).textTheme.bodyMedium,
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
