import 'dart:async';

import 'package:flutter/material.dart';

class SplashGate extends StatefulWidget {
  const SplashGate({
    super.key,
    required this.child,
  });

  final Widget child;

  @override
  State<SplashGate> createState() => _SplashGateState();
}

class _SplashGateState extends State<SplashGate> {
  var _showSplash = true;
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _timer = Timer(const Duration(milliseconds: 1800), () {
      if (mounted) {
        setState(() {
          _showSplash = false;
        });
      }
    });
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (!_showSplash) {
      return widget.child;
    }

    return const _SplashScreen();
  }
}

class _SplashScreen extends StatelessWidget {
  const _SplashScreen();

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;

    return Scaffold(
      backgroundColor: colorScheme.surface,
      body: SafeArea(
        child: Center(
          child: Padding(
            padding: const EdgeInsets.all(28),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Container(
                  width: 84,
                  height: 84,
                  decoration: BoxDecoration(
                    color: colorScheme.primaryContainer,
                    shape: BoxShape.circle,
                  ),
                  child: Icon(
                    Icons.accessibility_new_rounded,
                    size: 44,
                    color: colorScheme.onPrimaryContainer,
                  ),
                ),
                const SizedBox(height: 24),
                Text(
                  'Tiny Vision Cane',
                  textAlign: TextAlign.center,
                  style: Theme.of(context).textTheme.headlineMedium?.copyWith(
                        fontWeight: FontWeight.w800,
                      ),
                ),
                const SizedBox(height: 10),
                Text(
                  '시각장애인을 위한 스마트 지팡이 서비스',
                  textAlign: TextAlign.center,
                  style: Theme.of(context).textTheme.titleMedium?.copyWith(
                        color: colorScheme.onSurfaceVariant,
                      ),
                ),
                const SizedBox(height: 28),
                SizedBox(
                  width: 120,
                  child: LinearProgressIndicator(
                    minHeight: 3,
                    borderRadius: BorderRadius.circular(999),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
