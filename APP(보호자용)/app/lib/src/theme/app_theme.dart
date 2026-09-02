import 'package:flutter/material.dart';

abstract final class AppColors {
  static const canvas = Color(0xFFFFF7F0);
  static const peachSoft = Color(0xFFFFE1CC);
  static const peach = Color(0xFFFFB38A);
  static const sky = Color(0xFF7AB8D6);
  static const navy = Color(0xFF2B3D4F);
  static const white = Color(0xFFFFFFFF);
  static const border = Color(0xFFE8D4C5);
  static const danger = Color(0xFFB3261E);
}

ThemeData buildAppTheme() {
  final scheme = ColorScheme.fromSeed(
    seedColor: AppColors.navy,
    brightness: Brightness.light,
  ).copyWith(
    primary: AppColors.navy,
    onPrimary: AppColors.white,
    primaryContainer: AppColors.sky,
    onPrimaryContainer: AppColors.navy,
    secondary: AppColors.peach,
    onSecondary: AppColors.navy,
    secondaryContainer: AppColors.peachSoft,
    onSecondaryContainer: AppColors.navy,
    tertiary: AppColors.sky,
    onTertiary: AppColors.navy,
    surface: AppColors.canvas,
    onSurface: AppColors.navy,
    surfaceContainerHighest: AppColors.peachSoft,
    outline: const Color(0xFF756B64),
    outlineVariant: AppColors.border,
    error: AppColors.danger,
  );

  final base = ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    scaffoldBackgroundColor: AppColors.canvas,
  );

  return base.copyWith(
    textTheme: base.textTheme.apply(
      bodyColor: AppColors.navy,
      displayColor: AppColors.navy,
    ),
    appBarTheme: const AppBarTheme(
      elevation: 0,
      scrolledUnderElevation: 0,
      centerTitle: false,
      backgroundColor: AppColors.canvas,
      foregroundColor: AppColors.navy,
      surfaceTintColor: Colors.transparent,
      titleTextStyle: TextStyle(
        color: AppColors.navy,
        fontSize: 21,
        fontWeight: FontWeight.w800,
      ),
    ),
    cardTheme: const CardThemeData(
      color: AppColors.white,
      surfaceTintColor: Colors.transparent,
      elevation: 1,
      shadowColor: Color(0x1A2B3D4F),
      margin: EdgeInsets.zero,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.all(Radius.circular(18)),
        side: BorderSide(color: AppColors.border),
      ),
    ),
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
        backgroundColor: AppColors.navy,
        foregroundColor: AppColors.white,
        minimumSize: const Size(0, 50),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(14),
        ),
        textStyle: const TextStyle(fontWeight: FontWeight.w700),
      ),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(
        foregroundColor: AppColors.navy,
        side: const BorderSide(color: AppColors.sky, width: 1.4),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(14),
        ),
        textStyle: const TextStyle(fontWeight: FontWeight.w700),
      ),
    ),
    inputDecorationTheme: const InputDecorationTheme(
      filled: true,
      fillColor: AppColors.white,
      border: OutlineInputBorder(
        borderRadius: BorderRadius.all(Radius.circular(14)),
      ),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.all(Radius.circular(14)),
        borderSide: BorderSide(color: AppColors.border),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.all(Radius.circular(14)),
        borderSide: BorderSide(color: AppColors.sky, width: 2),
      ),
      prefixIconColor: AppColors.navy,
    ),
    progressIndicatorTheme: const ProgressIndicatorThemeData(
      color: AppColors.sky,
      linearTrackColor: AppColors.peachSoft,
    ),
    snackBarTheme: const SnackBarThemeData(
      backgroundColor: AppColors.navy,
      contentTextStyle: TextStyle(color: AppColors.white),
      behavior: SnackBarBehavior.floating,
    ),
    dividerTheme: const DividerThemeData(color: AppColors.border),
  );
}
