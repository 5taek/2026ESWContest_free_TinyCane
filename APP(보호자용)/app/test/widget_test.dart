import 'package:flutter_test/flutter_test.dart';

import 'package:care_guardian_app/src/ui/firebase_setup_app.dart';

void main() {
  testWidgets('shows Firebase setup guidance', (tester) async {
    await tester.pumpWidget(const FirebaseSetupApp(error: 'missing config'));

    expect(find.text('Firebase 설정 필요'), findsOneWidget);
    expect(find.textContaining('flutterfire configure'), findsOneWidget);
  });
}
