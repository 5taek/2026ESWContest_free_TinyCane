import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_messaging/firebase_messaging.dart';
import 'package:flutter/material.dart';

import 'firebase_options.dart';
import 'src/theme/app_theme.dart';
import 'src/ui/auth_gate.dart';
import 'src/ui/firebase_setup_app.dart';
import 'src/ui/splash_gate.dart';

@pragma('vm:entry-point')
Future<void> _firebaseMessagingBackgroundHandler(RemoteMessage message) async {
  // Runs in a separate background isolate with no surrounding UI to report
  // to -- an uncaught failure here would otherwise just vanish silently.
  try {
    await Firebase.initializeApp(
      options: DefaultFirebaseOptions.currentPlatform,
    );
  } catch (error, stackTrace) {
    debugPrint(
      'Background FCM handler failed to initialize Firebase: $error\n'
      '$stackTrace',
    );
  }
}

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  try {
    await Firebase.initializeApp(
      options: DefaultFirebaseOptions.currentPlatform,
    );

    FirebaseMessaging.onBackgroundMessage(_firebaseMessagingBackgroundHandler);

    runApp(const CareGuardianApp());
  } catch (error, stackTrace) {
    FlutterError.reportError(
      FlutterErrorDetails(exception: error, stack: stackTrace),
    );
    runApp(FirebaseSetupApp(error: error));
  }
}

class CareGuardianApp extends StatelessWidget {
  const CareGuardianApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Tiny Vision Cane',
      theme: buildAppTheme(),
      home: const SplashGate(child: AuthGate()),
    );
  }
}
