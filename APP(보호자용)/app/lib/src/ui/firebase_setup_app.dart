import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

class FirebaseSetupApp extends StatelessWidget {
  const FirebaseSetupApp({super.key, required this.error});

  final Object error;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildAppTheme(),
      home: Scaffold(
        backgroundColor: AppColors.canvas,
        appBar: AppBar(
          title: const Text('Tiny Vision Cane'),
          backgroundColor: AppColors.canvas,
        ),
        body: SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(20),
            child: Center(
              child: ConstrainedBox(
                constraints: const BoxConstraints(maxWidth: 520),
                child: Card(
                  elevation: 0,
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(18),
                    side: const BorderSide(color: AppColors.border),
                  ),
                  child: Padding(
                    padding: const EdgeInsets.all(20),
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Icon(
                          Icons.settings_rounded,
                          color: AppColors.danger,
                          size: 36,
                        ),
                        const SizedBox(height: 16),
                        Text(
                          'Firebase 설정 필요',
                          style: Theme.of(context).textTheme.titleLarge,
                        ),
                        const SizedBox(height: 8),
                        const Text(
                          'app 폴더에서 flutterfire configure를 실행한 뒤 앱을 다시 시작하세요.',
                        ),
                        const SizedBox(height: 16),
                        SelectableText(
                          error.toString(),
                          style: Theme.of(context).textTheme.bodySmall,
                        ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
