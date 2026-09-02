import 'package:firebase_core/firebase_core.dart';
import 'package:flutter/foundation.dart';

/// Temporary Firebase options.
///
/// Replace this file by running `flutterfire configure` from the app directory.
/// The environment variable fallback is only for early local experiments.
class DefaultFirebaseOptions {
  static FirebaseOptions get currentPlatform {
    if (kIsWeb) {
      throw UnsupportedError('This prototype is configured for Android first.');
    }

    switch (defaultTargetPlatform) {
      case TargetPlatform.android:
        _validate(android);
        return android;
      case TargetPlatform.iOS:
      case TargetPlatform.macOS:
      case TargetPlatform.windows:
      case TargetPlatform.linux:
      case TargetPlatform.fuchsia:
        throw UnsupportedError('This prototype is configured for Android first.');
    }
  }

  static const FirebaseOptions android = FirebaseOptions(
    apiKey: 'AIzaSyA-l5z-XgfCOr6vuRqx2vSTZB8TGDDiRqg',
    appId: '1:296288710255:android:4b4e4808d58c355f04e79a',
    messagingSenderId: '296288710255',
    projectId: 'tiny-vision-cane-6e80d',
    storageBucket: 'tiny-vision-cane-6e80d.firebasestorage.app',
  );
  static void _validate(FirebaseOptions options) {
    final missing = <String>[
      if (options.apiKey.isEmpty) 'FIREBASE_API_KEY',
      if (options.appId.isEmpty) 'FIREBASE_APP_ID',
      if (options.messagingSenderId.isEmpty) 'FIREBASE_MESSAGING_SENDER_ID',
      if (options.projectId.isEmpty) 'FIREBASE_PROJECT_ID',
    ];

    if (missing.isNotEmpty) {
      throw StateError(
        'Firebase settings are missing. Run `flutterfire configure` or pass '
        '--dart-define values. Missing: ${missing.join(', ')}',
      );
    }
  }
}
