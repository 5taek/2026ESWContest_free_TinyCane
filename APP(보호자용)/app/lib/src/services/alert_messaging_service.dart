import 'dart:async';

import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_messaging/firebase_messaging.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

class AlertMessagingService {
  AlertMessagingService({
    FirebaseMessaging? messaging,
    FlutterLocalNotificationsPlugin? localNotifications,
  })  : _messaging = messaging ?? FirebaseMessaging.instance,
        _localNotifications =
            localNotifications ?? FlutterLocalNotificationsPlugin();

  static const channelId = 'fall_alerts';

  static const AndroidNotificationChannel _androidChannel =
      AndroidNotificationChannel(
    channelId,
    '낙상 알림',
    description: '낙상 및 지팡이 상태 알림',
    importance: Importance.max,
  );

  final FirebaseMessaging _messaging;
  final FlutterLocalNotificationsPlugin _localNotifications;
  StreamSubscription<String>? _tokenRefreshSubscription;
  StreamSubscription<RemoteMessage>? _foregroundMessageSubscription;
  Timer? _tokenRegistrationRetryTimer;
  String? _activeUserId;
  var _tokenRegistrationRetryAttempt = 0;

  static const _maxTokenRetryDelay = Duration(minutes: 5);

  Future<void> initialize({required String userId}) async {
    _activeUserId = userId;
    _tokenRegistrationRetryTimer?.cancel();
    _tokenRegistrationRetryTimer = null;
    _tokenRegistrationRetryAttempt = 0;

    await _createAndroidChannel();
    await _initializeLocalNotifications();

    final settings = await _messaging.requestPermission(
      alert: true,
      badge: true,
      sound: true,
      provisional: false,
    );
    if (settings.authorizationStatus == AuthorizationStatus.denied) {
      // Alerts are the entire point of this app -- a denied permission means
      // the guardian will never see a push notification, so this must not
      // fail silently. permissionStatus() lets the UI surface it explicitly
      // instead of showing the same "대기 중" state as a healthy connection.
      debugPrint(
        'AlertMessagingService: notification permission denied for user '
        '$userId -- FCM pushes will not be delivered.',
      );
    }

    await _messaging.setForegroundNotificationPresentationOptions(
      alert: true,
      badge: true,
      sound: true,
    );

    await _registerDeviceToken(userId);
    _tokenRefreshSubscription?.cancel();
    _tokenRefreshSubscription = _messaging.onTokenRefresh.listen(
      (token) async {
        try {
          await _storeDeviceToken(userId: userId, token: token);
        } catch (error, stackTrace) {
          debugPrint(
            'AlertMessagingService: failed to store refreshed token: $error\n'
            '$stackTrace',
          );
          _scheduleTokenRegistrationRetry(userId);
        }
      },
    );

    _foregroundMessageSubscription?.cancel();
    _foregroundMessageSubscription =
        FirebaseMessaging.onMessage.listen(_showForegroundNotification);
  }

  Future<String?> deviceToken() {
    return _messaging.getToken();
  }

  /// Current OS-level notification permission, so the UI can distinguish
  /// "waiting for the first push" from "the guardian will never get a push
  /// because permission was denied" -- requestPermission()'s own return
  /// value from initialize() is not enough on its own since the user can
  /// also revoke permission later from system settings.
  Future<AuthorizationStatus> permissionStatus() {
    return _messaging
        .getNotificationSettings()
        .then((settings) => settings.authorizationStatus);
  }

  /// Re-prompts for permission. On Android this re-shows the system dialog
  /// as long as the guardian hasn't ticked "don't ask again"; harmless no-op
  /// if already granted.
  Future<AuthorizationStatus> requestPermissionAgain() async {
    final settings = await _messaging.requestPermission(
      alert: true,
      badge: true,
      sound: true,
      provisional: false,
    );
    return settings.authorizationStatus;
  }

  void dispose() {
    _activeUserId = null;
    _tokenRegistrationRetryTimer?.cancel();
    _tokenRegistrationRetryTimer = null;
    _tokenRegistrationRetryAttempt = 0;
    _tokenRefreshSubscription?.cancel();
    _tokenRefreshSubscription = null;
    _foregroundMessageSubscription?.cancel();
    _foregroundMessageSubscription = null;
  }

  Future<void> _createAndroidChannel() async {
    final androidNotifications =
        _localNotifications.resolvePlatformSpecificImplementation<
            AndroidFlutterLocalNotificationsPlugin>();

    await androidNotifications?.createNotificationChannel(_androidChannel);
  }

  Future<void> _initializeLocalNotifications() async {
    const initializationSettings = InitializationSettings(
      android: AndroidInitializationSettings('@mipmap/ic_launcher'),
      iOS: DarwinInitializationSettings(
        requestAlertPermission: false,
        requestBadgePermission: false,
        requestSoundPermission: false,
      ),
    );

    await _localNotifications.initialize(settings: initializationSettings);
  }

  Future<void> _registerDeviceToken(String userId) async {
    if (_activeUserId != userId) {
      return;
    }

    try {
      final token = await _messaging.getToken();
      if (token == null) {
        throw StateError('FCM getToken() returned null');
      }

      await _storeDeviceToken(userId: userId, token: token);
      _tokenRegistrationRetryTimer?.cancel();
      _tokenRegistrationRetryTimer = null;
      _tokenRegistrationRetryAttempt = 0;
      debugPrint(
        'AlertMessagingService: FCM token registered for user $userId.',
      );
    } catch (error, stackTrace) {
      debugPrint(
        'AlertMessagingService: FCM token registration failed for user '
        '$userId: $error\n$stackTrace',
      );
      _scheduleTokenRegistrationRetry(userId);
    }
  }

  void _scheduleTokenRegistrationRetry(String userId) {
    if (_activeUserId != userId ||
        _tokenRegistrationRetryTimer?.isActive == true) {
      return;
    }

    final exponent = _tokenRegistrationRetryAttempt.clamp(0, 8);
    final seconds = 1 << exponent;
    final delay = Duration(
      seconds: seconds > _maxTokenRetryDelay.inSeconds
          ? _maxTokenRetryDelay.inSeconds
          : seconds,
    );
    _tokenRegistrationRetryAttempt++;
    debugPrint(
      'AlertMessagingService: retrying FCM token registration in '
      '${delay.inSeconds}s (attempt $_tokenRegistrationRetryAttempt).',
    );
    _tokenRegistrationRetryTimer = Timer(delay, () {
      _tokenRegistrationRetryTimer = null;
      unawaited(_registerDeviceToken(userId));
    });
  }

  Future<void> _storeDeviceToken({
    required String userId,
    required String token,
  }) async {
    await FirebaseFirestore.instance
        .collection('users')
        .doc(userId)
        .collection('fcmTokens')
        .doc('current')
        .set({
      'token': token,
      'platform': 'android',
      'updatedAt': FieldValue.serverTimestamp(),
    }, SetOptions(merge: true));
  }

  Future<void> _showForegroundNotification(RemoteMessage message) async {
    final notification = message.notification;
    final title =
        notification?.title ?? message.data['title'] ?? '낙상 위험 감지';
    final body = notification?.body ??
        message.data['body'] ??
        message.data['message'] ??
        '보호 대상자의 위험 상태가 감지되었습니다.';

    final notificationId =
        DateTime.now().millisecondsSinceEpoch.remainder(2147483647);

    await _localNotifications.show(
      id: notificationId,
      title: title,
      body: body,
      notificationDetails: const NotificationDetails(
        android: AndroidNotificationDetails(
          channelId,
          '낙상 알림',
          channelDescription: '낙상 및 지팡이 상태 알림',
          importance: Importance.max,
          priority: Priority.max,
          category: AndroidNotificationCategory.alarm,
        ),
        iOS: DarwinNotificationDetails(
          presentAlert: true,
          presentBadge: true,
          presentSound: true,
        ),
      ),
      payload: message.data['eventId'],
    );
  }
}
