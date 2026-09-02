import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';

import '../services/alert_messaging_service.dart';
import 'home_page.dart';
import 'login_page.dart';

class AuthGate extends StatelessWidget {
  const AuthGate({super.key});

  @override
  Widget build(BuildContext context) {
    return StreamBuilder<User?>(
      stream: FirebaseAuth.instance.authStateChanges(),
      builder: (context, snapshot) {
        if (snapshot.connectionState == ConnectionState.waiting) {
          return const Scaffold(
            body: Center(child: CircularProgressIndicator()),
          );
        }

        final user = snapshot.data;
        if (user == null) {
          return const LoginPage();
        }

        return _AuthenticatedHome(user: user);
      },
    );
  }
}

class _AuthenticatedHome extends StatefulWidget {
  const _AuthenticatedHome({required this.user});

  final User user;

  @override
  State<_AuthenticatedHome> createState() => _AuthenticatedHomeState();
}

class _AuthenticatedHomeState extends State<_AuthenticatedHome> {
  late AlertMessagingService _messagingService;
  late Future<void> _messagingInit;

  @override
  void initState() {
    super.initState();
    _messagingService = AlertMessagingService();
    _messagingInit = _messagingService.initialize(userId: widget.user.uid);
  }

  @override
  void didUpdateWidget(covariant _AuthenticatedHome oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.user.uid != widget.user.uid) {
      _messagingService.dispose();
      _messagingService = AlertMessagingService();
      _messagingInit = _messagingService.initialize(userId: widget.user.uid);
    }
  }

  @override
  void dispose() {
    _messagingService.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return FutureBuilder<void>(
      future: _messagingInit,
      builder: (context, snapshot) {
        return HomePage(
          user: widget.user,
          messagingService: _messagingService,
          messagingError: snapshot.error,
        );
      },
    );
  }
}
