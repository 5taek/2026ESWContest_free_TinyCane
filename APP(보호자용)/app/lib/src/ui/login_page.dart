import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

class LoginPage extends StatefulWidget {
  const LoginPage({super.key});

  @override
  State<LoginPage> createState() => _LoginPageState();
}

class _LoginPageState extends State<LoginPage> {
  final _guardianIdController = TextEditingController();
  final _passwordController = TextEditingController();
  bool _isSubmitting = false;
  String? _errorMessage;

  @override
  void dispose() {
    _guardianIdController.dispose();
    _passwordController.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    final guardianId = _normalizeId(_guardianIdController.text);
    final password = _passwordController.text;

    final validationError = _validateGuardianCredentials(
      guardianId: guardianId,
      password: password,
    );
    if (validationError != null) {
      setState(() {
        _errorMessage = validationError;
      });
      return;
    }

    setState(() {
      _isSubmitting = true;
      _errorMessage = null;
    });

    try {
      final credential = await FirebaseAuth.instance.signInWithEmailAndPassword(
        email: _authEmail(guardianId),
        password: password,
      );

      await _ensureGuardianProfile(
        uid: credential.user!.uid,
        guardianId: guardianId,
      );
    } on FirebaseAuthException catch (error) {
      setState(() {
        _errorMessage = _authErrorText(error);
      });
    } catch (error) {
      setState(() {
        _errorMessage = '로그인 중 오류가 발생했습니다: $error';
      });
    } finally {
      if (mounted) {
        setState(() {
          _isSubmitting = false;
        });
      }
    }
  }

  void _openPatientRegistration() {
    Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (_) => const PatientRegistrationPage(),
      ),
    );
  }

  void _openGuardianRegistration() {
    Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (_) => const GuardianRegistrationPage(),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return _AuthFrame(
      title: '보호자 로그인',
      subtitle: 'Tiny Vision Cane',
      children: [
        TextField(
          controller: _guardianIdController,
          keyboardType: TextInputType.text,
          textInputAction: TextInputAction.next,
          autofillHints: const [AutofillHints.username],
          decoration: _authInputDecoration(
            labelText: '보호자 ID',
            helperText: '앱 로그인에 사용할 ID',
            icon: Icons.person_outline_rounded,
          ),
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _passwordController,
          obscureText: true,
          autofillHints: const [AutofillHints.password],
          decoration: _authInputDecoration(
            labelText: '비밀번호',
            icon: Icons.lock_outline_rounded,
          ),
          onSubmitted: (_) => _isSubmitting ? null : _submit(),
        ),
        if (_errorMessage != null) ...[
          const SizedBox(height: 12),
          _ErrorText(_errorMessage!),
        ],
        const SizedBox(height: 20),
        SizedBox(
          height: 48,
          child: FilledButton.icon(
            onPressed: _isSubmitting ? null : _submit,
            icon: _SubmitIcon(
              isSubmitting: _isSubmitting,
              idleIcon: Icons.login_rounded,
            ),
            label: const Text('로그인'),
          ),
        ),
        const SizedBox(height: 12),
        SizedBox(
          height: 48,
          child: OutlinedButton.icon(
            onPressed: _isSubmitting ? null : _openPatientRegistration,
            icon: const Icon(Icons.badge_outlined),
            label: const Text('사용자 등록'),
          ),
        ),
        const SizedBox(height: 10),
        SizedBox(
          height: 48,
          child: OutlinedButton.icon(
            onPressed: _isSubmitting ? null : _openGuardianRegistration,
            icon: const Icon(Icons.person_add_alt_1_rounded),
            label: const Text('보호자 등록'),
          ),
        ),
      ],
    );
  }
}

class PatientRegistrationPage extends StatefulWidget {
  const PatientRegistrationPage({super.key});

  @override
  State<PatientRegistrationPage> createState() =>
      _PatientRegistrationPageState();
}

class _PatientRegistrationPageState extends State<PatientRegistrationPage> {
  final _patientIdController = TextEditingController();
  final _deviceIdController = TextEditingController();
  bool _isSubmitting = false;
  bool _isResetting = false;
  String? _errorMessage;

  @override
  void dispose() {
    _patientIdController.dispose();
    _deviceIdController.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    final patientId = _normalizeId(_patientIdController.text);
    final deviceId = _deviceIdController.text.trim();

    final validationError = _validatePatientDevice(
      patientId: patientId,
      deviceId: deviceId,
    );
    if (validationError != null) {
      setState(() {
        _errorMessage = validationError;
      });
      return;
    }

    setState(() {
      _isSubmitting = true;
      _errorMessage = null;
    });

    try {
      await _registerPatientDevice(patientId: patientId, deviceId: deviceId);

      if (!mounted) {
        return;
      }

      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text('사용자와 기기가 등록되었습니다. 보호자 등록을 이어서 진행하세요.'),
        ),
      );
      Navigator.of(context).pushReplacement(
        MaterialPageRoute<void>(
          builder: (_) => GuardianRegistrationPage(
            initialPatientId: patientId,
          ),
        ),
      );
    } catch (error) {
      setState(() {
        _errorMessage = '사용자 등록 실패: $error';
      });
    } finally {
      if (mounted) {
        setState(() {
          _isSubmitting = false;
        });
      }
    }
  }

  Future<void> _resetRegistration() async {
    final patientId = _normalizeId(_patientIdController.text);
    final deviceId = _deviceIdController.text.trim();

    final validationError = _validatePatientDevice(
      patientId: patientId,
      deviceId: deviceId,
    );
    if (validationError != null) {
      setState(() {
        _errorMessage = validationError;
      });
      return;
    }

    final confirmed = await showDialog<bool>(
      context: context,
      builder: (dialogContext) {
        return AlertDialog(
          icon: Icon(
            Icons.restart_alt_rounded,
            color: Theme.of(dialogContext).colorScheme.error,
          ),
          title: const Text('사용자/기기 초기화'),
          content: const Text(
            '입력한 사용자 ID와 기기 ID의 기존 연결을 해제합니다. '
            '초기화 후 같은 기기를 다른 사용자에게 다시 등록할 수 있습니다.',
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(dialogContext).pop(false),
              child: const Text('취소'),
            ),
            FilledButton(
              onPressed: () => Navigator.of(dialogContext).pop(true),
              child: const Text('초기화'),
            ),
          ],
        );
      },
    );

    if (confirmed != true || !mounted) {
      return;
    }

    setState(() {
      _isResetting = true;
      _errorMessage = null;
    });

    try {
      await _resetPatientDeviceRegistration(
        patientId: patientId,
        deviceId: deviceId,
      );

      if (!mounted) {
        return;
      }

      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('사용자와 기기 연결을 초기화했습니다.')),
      );
    } catch (error) {
      setState(() {
        _errorMessage = '초기화 실패: $error';
      });
    } finally {
      if (mounted) {
        setState(() {
          _isResetting = false;
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return _AuthFrame(
      title: '사용자 등록',
      subtitle: '기기 연결',
      showBackButton: true,
      children: [
        TextField(
          controller: _patientIdController,
          keyboardType: TextInputType.text,
          textInputAction: TextInputAction.next,
          decoration: _authInputDecoration(
            labelText: '사용자 ID',
            helperText: '지팡이 사용자 또는 보호 대상자 ID',
            icon: Icons.accessibility_new_rounded,
          ),
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _deviceIdController,
          keyboardType: TextInputType.text,
          textInputAction: TextInputAction.done,
          decoration: _authInputDecoration(
            labelText: '기기 ID',
            helperText: 'Firebase에 등록된 지팡이 기기 번호',
            icon: Icons.sensors_rounded,
          ),
          onSubmitted: (_) =>
              _isSubmitting || _isResetting ? null : _submit(),
        ),
        if (_errorMessage != null) ...[
          const SizedBox(height: 12),
          _ErrorText(_errorMessage!),
        ],
        const SizedBox(height: 20),
        SizedBox(
          height: 48,
          child: FilledButton.icon(
            onPressed: _isSubmitting || _isResetting ? null : _submit,
            icon: _SubmitIcon(
              isSubmitting: _isSubmitting,
              idleIcon: Icons.badge_outlined,
            ),
            label: const Text('사용자 등록'),
          ),
        ),
        const SizedBox(height: 10),
        SizedBox(
          height: 48,
          child: OutlinedButton.icon(
            onPressed:
                _isSubmitting || _isResetting ? null : _resetRegistration,
            icon: _SubmitIcon(
              isSubmitting: _isResetting,
              idleIcon: Icons.restart_alt_rounded,
            ),
            label: const Text('사용자/기기 초기화'),
          ),
        ),
      ],
    );
  }
}

class GuardianRegistrationPage extends StatefulWidget {
  const GuardianRegistrationPage({
    super.key,
    this.initialPatientId,
  });

  final String? initialPatientId;

  @override
  State<GuardianRegistrationPage> createState() =>
      _GuardianRegistrationPageState();
}

class _GuardianRegistrationPageState extends State<GuardianRegistrationPage> {
  final _patientIdController = TextEditingController();
  final _guardianIdController = TextEditingController();
  final _passwordController = TextEditingController();
  bool _isSubmitting = false;
  String? _errorMessage;

  @override
  void initState() {
    super.initState();
    _patientIdController.text = widget.initialPatientId ?? '';
  }

  @override
  void dispose() {
    _patientIdController.dispose();
    _guardianIdController.dispose();
    _passwordController.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    final patientId = _normalizeId(_patientIdController.text);
    final guardianId = _normalizeId(_guardianIdController.text);
    final password = _passwordController.text;

    final validationError = _validateGuardianRegistration(
      patientId: patientId,
      guardianId: guardianId,
      password: password,
    );
    if (validationError != null) {
      setState(() {
        _errorMessage = validationError;
      });
      return;
    }

    setState(() {
      _isSubmitting = true;
      _errorMessage = null;
    });

    try {
      final credential =
          await FirebaseAuth.instance.createUserWithEmailAndPassword(
        email: _authEmail(guardianId),
        password: password,
      );

      await _createGuardianProfile(
        uid: credential.user!.uid,
        guardianId: guardianId,
        patientId: patientId,
      );

      // reportFall rejects with a 409 if devices/{deviceId} ends up without
      // both patientId and a non-empty guardianUids -- a guardian would
      // otherwise only discover a broken link much later, when a real fall
      // event silently fails to reach them. Catch it here instead, right
      // after registration, while it's still actionable.
      final linkWarning = await _verifyDeviceLink(
        patientId: patientId,
        uid: credential.user!.uid,
      );

      if (mounted) {
        if (linkWarning != null) {
          await showDialog<void>(
            context: context,
            builder: (dialogContext) => AlertDialog(
              icon: Icon(
                Icons.warning_amber_rounded,
                color: Theme.of(dialogContext).colorScheme.error,
              ),
              title: const Text('연결 확인 필요'),
              content: Text(linkWarning),
              actions: [
                TextButton(
                  onPressed: () => Navigator.of(dialogContext).pop(),
                  child: const Text('확인'),
                ),
              ],
            ),
          );
        }

        if (mounted) {
          Navigator.of(context).popUntil((route) => route.isFirst);
        }
      }
    } on FirebaseAuthException catch (error) {
      setState(() {
        _errorMessage = _authErrorText(error);
      });
    } catch (error) {
      setState(() {
        _errorMessage = '보호자 등록 중 오류가 발생했습니다: $error';
      });
    } finally {
      if (mounted) {
        setState(() {
          _isSubmitting = false;
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return _AuthFrame(
      title: '보호자 등록',
      subtitle: '계정 생성',
      showBackButton: true,
      children: [
        TextField(
          controller: _patientIdController,
          keyboardType: TextInputType.text,
          textInputAction: TextInputAction.next,
          decoration: _authInputDecoration(
            labelText: '사용자 ID',
            helperText: '연결할 지팡이 사용자 또는 보호 대상자 ID',
            icon: Icons.accessibility_new_rounded,
          ),
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _guardianIdController,
          keyboardType: TextInputType.text,
          textInputAction: TextInputAction.next,
          autofillHints: const [AutofillHints.username],
          decoration: _authInputDecoration(
            labelText: '보호자 ID',
            helperText: '앱 로그인에 사용할 ID',
            icon: Icons.person_outline_rounded,
          ),
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _passwordController,
          obscureText: true,
          autofillHints: const [AutofillHints.password],
          decoration: _authInputDecoration(
            labelText: '비밀번호',
            icon: Icons.lock_outline_rounded,
          ),
          onSubmitted: (_) => _isSubmitting ? null : _submit(),
        ),
        if (_errorMessage != null) ...[
          const SizedBox(height: 12),
          _ErrorText(_errorMessage!),
        ],
        const SizedBox(height: 20),
        SizedBox(
          height: 48,
          child: FilledButton.icon(
            onPressed: _isSubmitting ? null : _submit,
            icon: _SubmitIcon(
              isSubmitting: _isSubmitting,
              idleIcon: Icons.person_add_alt_1_rounded,
            ),
            label: const Text('보호자 등록'),
          ),
        ),
      ],
    );
  }
}

class _AuthFrame extends StatelessWidget {
  const _AuthFrame({
    required this.title,
    required this.subtitle,
    required this.children,
    this.showBackButton = false,
  });

  final String title;
  final String subtitle;
  final List<Widget> children;
  final bool showBackButton;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Theme.of(context).colorScheme.surface,
      body: SafeArea(
        child: Stack(
          children: [
            Center(
              child: SingleChildScrollView(
                padding: const EdgeInsets.all(20),
                child: ConstrainedBox(
                  constraints: const BoxConstraints(maxWidth: 420),
                  child: SizedBox(
                    width: double.infinity,
                    child: Card(
                      child: ConstrainedBox(
                        constraints: const BoxConstraints(minHeight: 500),
                        child: Padding(
                          padding: const EdgeInsets.all(20),
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Icon(
                                Icons.health_and_safety_rounded,
                                size: 44,
                                color: Theme.of(context).colorScheme.primary,
                              ),
                              const SizedBox(height: 16),
                              Text(
                                title,
                                textAlign: TextAlign.center,
                                style:
                                    Theme.of(context).textTheme.headlineSmall,
                              ),
                              const SizedBox(height: 6),
                              Text(
                                subtitle,
                                textAlign: TextAlign.center,
                                style: Theme.of(context).textTheme.bodyMedium,
                              ),
                              const SizedBox(height: 20),
                              ...children,
                            ],
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
            if (showBackButton)
              Positioned(
                left: 8,
                top: 8,
                child: IconButton(
                  tooltip: '뒤로',
                  onPressed: () => Navigator.of(context).maybePop(),
                  icon: const Icon(Icons.arrow_back_rounded),
                ),
              ),
          ],
        ),
      ),
    );
  }
}

InputDecoration _authInputDecoration({
  required String labelText,
  required IconData icon,
  String? helperText,
}) {
  const borderRadius = BorderRadius.all(Radius.circular(14));
  return InputDecoration(
    labelText: labelText,
    helperText: helperText,
    helperMaxLines: 2,
    prefixIcon: Icon(icon),
    filled: true,
    fillColor: AppColors.white,
    contentPadding: const EdgeInsets.symmetric(horizontal: 14, vertical: 16),
    border: const OutlineInputBorder(
      borderRadius: borderRadius,
    ),
    enabledBorder: const OutlineInputBorder(
      borderRadius: borderRadius,
      borderSide: BorderSide(color: AppColors.border),
    ),
    focusedBorder: const OutlineInputBorder(
      borderRadius: borderRadius,
      borderSide: BorderSide(color: AppColors.sky, width: 2),
    ),
  );
}

class _SubmitIcon extends StatelessWidget {
  const _SubmitIcon({
    required this.isSubmitting,
    required this.idleIcon,
  });

  final bool isSubmitting;
  final IconData idleIcon;

  @override
  Widget build(BuildContext context) {
    if (!isSubmitting) {
      return Icon(idleIcon);
    }

    return const SizedBox(
      width: 18,
      height: 18,
      child: CircularProgressIndicator(strokeWidth: 2),
    );
  }
}

class _ErrorText extends StatelessWidget {
  const _ErrorText(this.message);

  final String message;

  @override
  Widget build(BuildContext context) {
    return Text(
      message,
      style: TextStyle(color: Theme.of(context).colorScheme.error),
    );
  }
}

class _RegistrationException implements Exception {
  const _RegistrationException(this.message);

  final String message;

  @override
  String toString() => message;
}

String? _validateGuardianCredentials({
  required String guardianId,
  required String password,
}) {
  if (guardianId.isEmpty || password.isEmpty) {
    return '보호자 ID와 비밀번호를 입력하세요.';
  }
  if (!_idPattern.hasMatch(guardianId)) {
    return '보호자 ID는 영문 소문자, 숫자, ., _, - 조합 3~32자로 입력하세요.';
  }
  if (password.length < 6) {
    return '비밀번호는 6자 이상이어야 합니다.';
  }

  return null;
}

String? _validatePatientDevice({
  required String patientId,
  required String deviceId,
}) {
  if (patientId.isEmpty) {
    return '사용자 ID를 입력하세요.';
  }
  if (!_idPattern.hasMatch(patientId)) {
    return '사용자 ID는 영문 소문자, 숫자, ., _, - 조합 3~32자로 입력하세요.';
  }
  if (deviceId.isEmpty) {
    return '기기 ID를 입력하세요.';
  }
  if (!_devicePattern.hasMatch(deviceId)) {
    return '기기 ID는 영문, 숫자, ., _, - 조합 2~64자로 입력하세요.';
  }

  return null;
}

String? _validateGuardianRegistration({
  required String patientId,
  required String guardianId,
  required String password,
}) {
  if (patientId.isEmpty) {
    return '사용자 ID를 입력하세요.';
  }
  if (!_idPattern.hasMatch(patientId)) {
    return '사용자 ID는 영문 소문자, 숫자, ., _, - 조합 3~32자로 입력하세요.';
  }

  return _validateGuardianCredentials(
    guardianId: guardianId,
    password: password,
  );
}

Future<void> _registerPatientDevice({
  required String patientId,
  required String deviceId,
}) async {
  final firestore = FirebaseFirestore.instance;
  final patientRef = firestore.collection('patients').doc(patientId);
  final deviceRef = firestore.collection('devices').doc(deviceId);

  await firestore.runTransaction((transaction) async {
    final deviceSnapshot = await transaction.get(deviceRef);
    if (!deviceSnapshot.exists) {
      throw const _RegistrationException(
        '등록된 기기 ID가 없습니다. Firebase의 devices 문서 또는 라즈베리파이 기기 등록을 먼저 확인하세요.',
      );
    }

    final deviceData = deviceSnapshot.data() ?? <String, dynamic>{};
    final assignedPatientId = deviceData['patientId']?.toString();
    if (assignedPatientId != null &&
        assignedPatientId.isNotEmpty &&
        assignedPatientId != patientId) {
      throw const _RegistrationException('이미 다른 사용자에게 연결된 기기입니다.');
    }

    final patientSnapshot = await transaction.get(patientRef);
    final patientData = patientSnapshot.data() ?? <String, dynamic>{};
    final assignedDeviceId = patientData['deviceId']?.toString();
    if (assignedDeviceId != null &&
        assignedDeviceId.isNotEmpty &&
        assignedDeviceId != deviceId) {
      throw const _RegistrationException('이 사용자 ID는 이미 다른 기기에 연결되어 있습니다.');
    }

    final patientUpdate = <String, Object?>{
      'patientId': patientId,
      'deviceId': deviceId,
      'updatedAt': FieldValue.serverTimestamp(),
    };
    if (!patientSnapshot.exists) {
      patientUpdate['guardianUids'] = <String>[];
      patientUpdate['createdAt'] = FieldValue.serverTimestamp();
    }

    transaction.set(patientRef, patientUpdate, SetOptions(merge: true));
    transaction.set(
      deviceRef,
      {
        'deviceId': deviceId,
        'patientId': patientId,
        'updatedAt': FieldValue.serverTimestamp(),
      },
      SetOptions(merge: true),
    );
  });
}

Future<void> _resetPatientDeviceRegistration({
  required String patientId,
  required String deviceId,
}) async {
  final firestore = FirebaseFirestore.instance;
  final patientRef = firestore.collection('patients').doc(patientId);
  final deviceRef = firestore.collection('devices').doc(deviceId);

  await firestore.runTransaction((transaction) async {
    final deviceSnapshot = await transaction.get(deviceRef);
    if (!deviceSnapshot.exists) {
      throw const _RegistrationException(
        '등록된 기기 ID가 없습니다. Firebase의 devices 문서를 먼저 확인하세요.',
      );
    }

    final patientSnapshot = await transaction.get(patientRef);
    final deviceData = deviceSnapshot.data() ?? <String, dynamic>{};
    final patientData = patientSnapshot.data() ?? <String, dynamic>{};
    final assignedPatientId = deviceData['patientId']?.toString();
    final assignedDeviceId = patientData['deviceId']?.toString();

    if (assignedPatientId == null || assignedPatientId.isEmpty) {
      throw const _RegistrationException('이미 초기화된 기기입니다.');
    }
    if (assignedPatientId != patientId) {
      throw const _RegistrationException(
        '입력한 사용자 ID가 이 기기에 연결된 사용자와 다릅니다.',
      );
    }
    if (patientSnapshot.exists &&
        assignedDeviceId != null &&
        assignedDeviceId.isNotEmpty &&
        assignedDeviceId != deviceId) {
      throw const _RegistrationException(
        '입력한 기기 ID가 이 사용자에게 연결된 기기와 다릅니다.',
      );
    }

    transaction.update(deviceRef, {
      'patientId': FieldValue.delete(),
      'guardianUids': FieldValue.delete(),
      'resetPatientId': patientId,
      'updatedAt': FieldValue.serverTimestamp(),
      'resetAt': FieldValue.serverTimestamp(),
    });

    if (patientSnapshot.exists) {
      transaction.update(patientRef, {
        'deviceId': FieldValue.delete(),
        'guardianUids': FieldValue.delete(),
        'resetDeviceId': deviceId,
        'updatedAt': FieldValue.serverTimestamp(),
        'resetAt': FieldValue.serverTimestamp(),
      });
    }
  });
}

Future<void> _createGuardianProfile({
  required String uid,
  required String guardianId,
  required String patientId,
}) async {
  final firestore = FirebaseFirestore.instance;
  final guardianRef = firestore.collection('users').doc(uid);
  final patientRef = firestore.collection('patients').doc(patientId);
  final patientSnapshot = await patientRef.get();
  final patientData = patientSnapshot.data() ?? <String, dynamic>{};
  final deviceId = patientData['deviceId']?.toString();

  final batch = firestore.batch();
  final guardianData = <String, Object?>{
    'uid': uid,
    'role': 'guardian',
    'guardianId': guardianId,
    'patientIds': FieldValue.arrayUnion([patientId]),
    'createdAt': FieldValue.serverTimestamp(),
    'updatedAt': FieldValue.serverTimestamp(),
  };
  if (deviceId != null && deviceId.isNotEmpty) {
    guardianData['deviceIds'] = FieldValue.arrayUnion([deviceId]);
  }

  batch.set(
    guardianRef,
    guardianData,
    SetOptions(merge: true),
  );

  batch.set(
    patientRef,
    {
      'patientId': patientId,
      'guardianUids': FieldValue.arrayUnion([uid]),
      'createdAt': FieldValue.serverTimestamp(),
      'updatedAt': FieldValue.serverTimestamp(),
    },
    SetOptions(merge: true),
  );

  if (deviceId != null && deviceId.isNotEmpty) {
    batch.set(
      firestore.collection('devices').doc(deviceId),
      {
        'deviceId': deviceId,
        'patientId': patientId,
        'guardianUids': FieldValue.arrayUnion([uid]),
        'updatedAt': FieldValue.serverTimestamp(),
      },
      SetOptions(merge: true),
    );
  }

  await batch.commit();
}

/// Reads back patients/{patientId} and devices/{deviceId} directly (rather
/// than trusting that _createGuardianProfile's writes landed exactly as
/// expected) and returns a human-readable warning if the link is
/// incomplete, or null if everything checks out. This mirrors exactly the
/// condition functions/index.js's reportFall checks before accepting a
/// device's fall report (missing patientId or empty guardianUids -> 409).
Future<String?> _verifyDeviceLink({
  required String patientId,
  required String uid,
}) async {
  try {
    final firestore = FirebaseFirestore.instance;
    final patientSnapshot =
        await firestore.collection('patients').doc(patientId).get();
    final deviceId = patientSnapshot.data()?['deviceId']?.toString();
    if (deviceId == null || deviceId.isEmpty) {
      return '사용자($patientId)에 연결된 기기 ID를 찾을 수 없습니다. '
          '사용자 등록이 정상적으로 완료되었는지 확인해주세요.';
    }

    final deviceSnapshot =
        await firestore.collection('devices').doc(deviceId).get();
    final deviceData = deviceSnapshot.data();
    final linkedPatientId = deviceData?['patientId']?.toString();
    final rawGuardianUids = deviceData?['guardianUids'];
    final guardianUids = rawGuardianUids is Iterable
        ? rawGuardianUids.map((value) => value.toString()).toList()
        : const <String>[];

    if (linkedPatientId != patientId) {
      return '기기($deviceId)가 이 사용자($patientId)에 연결되어 있지 않습니다.';
    }
    if (!guardianUids.contains(uid)) {
      return '기기($deviceId)에 이 보호자 계정이 아직 연결되지 않았습니다. '
          '지팡이가 낙상을 감지해도 알림이 오지 않을 수 있으니 다시 등록을 시도해주세요.';
    }
    return null;
  } catch (error) {
    return '기기 연결 상태를 확인하지 못했습니다: $error';
  }
}

Future<void> _ensureGuardianProfile({
  required String uid,
  required String guardianId,
}) async {
  await FirebaseFirestore.instance.collection('users').doc(uid).set(
    {
      'uid': uid,
      'role': 'guardian',
      'guardianId': guardianId,
      'lastLoginAt': FieldValue.serverTimestamp(),
      'updatedAt': FieldValue.serverTimestamp(),
    },
    SetOptions(merge: true),
  );
}

String _authErrorText(FirebaseAuthException error) {
  final message = error.message ?? '';

  switch (error.code) {
    case 'email-already-in-use':
      return '이미 사용 중인 보호자 ID입니다.';
    case 'invalid-email':
      return '보호자 ID 형식이 올바르지 않습니다.';
    case 'user-not-found':
    case 'wrong-password':
    case 'invalid-credential':
      return '보호자 ID 또는 비밀번호가 올바르지 않습니다.';
    case 'weak-password':
      return '비밀번호는 6자 이상이어야 합니다.';
    case 'operation-not-allowed':
      return 'Firebase Console에서 이메일/비밀번호 로그인을 활성화해야 합니다.';
    case 'configuration-not-found':
      return 'Firebase Authentication 설정이 아직 활성화되지 않았습니다. Firebase Console에서 Authentication을 시작하고 이메일/비밀번호 로그인을 켜주세요.';
    case 'internal-error':
      if (message.contains('CONFIGURATION_NOT_FOUND')) {
        return 'Firebase Authentication 설정이 아직 활성화되지 않았습니다. Firebase Console에서 Authentication을 시작하고 이메일/비밀번호 로그인을 켜주세요.';
      }
      return message.isNotEmpty ? message : '인증 내부 오류가 발생했습니다.';
    default:
      return message.isNotEmpty ? message : '인증 오류가 발생했습니다.';
  }
}

String _normalizeId(String value) {
  return value.trim().toLowerCase();
}

String _authEmail(String guardianId) {
  return '$guardianId@$_authDomain';
}

const _authDomain = 'tinyvisioncane.local';
final _idPattern = RegExp(r'^[a-z0-9._-]{3,32}$');
final _devicePattern = RegExp(r'^[a-zA-Z0-9._-]{2,64}$');
