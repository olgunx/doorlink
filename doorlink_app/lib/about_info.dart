import 'dart:io' show Platform;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

class AboutInfo {
  static const String appName = "DoorLink";
  static const String appVersion = "1.0.0+1";
  static const String tagline = "Zero-Interaction Smart Entry & Access Control";
  static const String developer = "UNL Labs";
  static const String appDescription =
      "DoorLink is a secure, offline-first access control application. "
      "It pairs with the DoorLink smart lock via Bluetooth Low Energy (BLE) "
      "for seamless background authentication without cloud dependencies.";
  static const String deviceDescription =
      "DoorLink Hardware: An ESP32 smart controller featuring secure cryptographic handshakes "
      "(NIST P-256 ECDH, HMAC-SHA256), proximity sensing, and real-time offline access control.";
  static const String buildDate = "2026-07-05 13:48:42 +03:00";
  static const String commitHash = "ae14335";
  static const String securitySpecs =
      "• Cryptography: NIST P-256 ECDH Key Agreement & HMAC-SHA256\n"
      "• Challenge Rotation: Dynamic 15-second BLE rotating challenge\n"
      "• Proximity & Intent: RSSI signal threshold + Laser physical wave trigger\n"
      "• Anti-Replay: One-time challenge invalidation upon unlock\n"
      "• Zero Cloud: 100% local device-to-lock communication";

  static String getFormattedDiagnostics() {
    final os = Platform.operatingSystem;
    final osVersion = Platform.operatingSystemVersion;
    return "=== DoorLink System Diagnostics ===\n"
        "App: $appName v$appVersion\n"
        "Developer: $developer\n"
        "Build Date: $buildDate\n"
        "Commit: $commitHash\n"
        "Platform: $os ($osVersion)\n"
        "Architecture: Offline-First BLE Zero-Trust Security\n"
        "===================================";
  }
}

class AboutSection extends StatelessWidget {
  const AboutSection({super.key});

  void _copyDiagnostics(BuildContext context) {
    Clipboard.setData(ClipboardData(text: AboutInfo.getFormattedDiagnostics()));
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(
        content: Text('System diagnostics copied to clipboard.'),
        behavior: SnackBarBehavior.floating,
        duration: Duration(seconds: 2),
      ),
    );
  }

  void _showSecurityDialog(BuildContext context) {
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Row(
          children: [
            Icon(Icons.shield_outlined, color: Colors.blueGrey),
            SizedBox(width: 8),
            Text('Security Architecture'),
          ],
        ),
        content: const SingleChildScrollView(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              Text(
                'DoorLink Zero-Trust Protocol',
                style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16),
              ),
              SizedBox(height: 10),
              Text(
                AboutInfo.securitySpecs,
                style: TextStyle(height: 1.5),
              ),
            ],
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(),
            child: const Text('Close'),
          ),
        ],
      ),
    );
  }

  void _showLicensesDialog(BuildContext context) {
    showLicensePage(
      context: context,
      applicationName: AboutInfo.appName,
      applicationVersion: AboutInfo.appVersion,
      applicationIcon: const Padding(
        padding: EdgeInsets.all(12.0),
        child: Icon(Icons.door_back_door_outlined, size: 48, color: Colors.blueGrey),
      ),
      applicationLegalese: '© ${AboutInfo.developer}. All rights reserved.',
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Icon(Icons.info_outline, color: colorScheme.primary, size: 24),
            const SizedBox(width: 8),
            Text(
              'About',
              style: theme.textTheme.titleLarge?.copyWith(
                fontWeight: FontWeight.bold,
              ),
            ),
          ],
        ),
        const SizedBox(height: 12),
        Card(
          elevation: 2,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
            side: BorderSide(
              color: colorScheme.outlineVariant.withValues(alpha: 0.5),
            ),
          ),
          child: Padding(
            padding: const EdgeInsets.all(20.0),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                // Header Banner: Logo + App Name + Version Badges
                Row(
                  crossAxisAlignment: CrossAxisAlignment.center,
                  children: [
                    Container(
                      width: 56,
                      height: 56,
                      decoration: BoxDecoration(
                        gradient: LinearGradient(
                          colors: [
                            colorScheme.primary,
                            colorScheme.tertiary,
                          ],
                          begin: Alignment.topLeft,
                          end: Alignment.bottomRight,
                        ),
                        borderRadius: BorderRadius.circular(16),
                        boxShadow: [
                          BoxShadow(
                            color: colorScheme.primary.withValues(alpha: 0.25),
                            blurRadius: 10,
                            offset: const Offset(0, 4),
                          ),
                        ],
                      ),
                      child: Icon(
                        Icons.door_back_door_outlined,
                        color: colorScheme.onPrimary,
                        size: 32,
                      ),
                    ),
                    const SizedBox(width: 16),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(
                            children: [
                              Text(
                                AboutInfo.appName,
                                style: theme.textTheme.headlineSmall?.copyWith(
                                  fontWeight: FontWeight.bold,
                                  letterSpacing: -0.5,
                                ),
                              ),
                              const SizedBox(width: 8),
                              Container(
                                padding: const EdgeInsets.symmetric(
                                    horizontal: 8, vertical: 2),
                                decoration: BoxDecoration(
                                  color: colorScheme.primaryContainer,
                                  borderRadius: BorderRadius.circular(12),
                                ),
                                child: Text(
                                  'v${AboutInfo.appVersion}',
                                  style: theme.textTheme.labelSmall?.copyWith(
                                    color: colorScheme.onPrimaryContainer,
                                    fontWeight: FontWeight.bold,
                                  ),
                                ),
                              ),
                            ],
                          ),
                          const SizedBox(height: 4),
                          Text(
                            AboutInfo.tagline,
                            style: theme.textTheme.bodyMedium?.copyWith(
                              color: colorScheme.onSurfaceVariant,
                            ),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),

                const SizedBox(height: 20),
                const Divider(),
                const SizedBox(height: 16),

                // Core Descriptions
                Text(
                  'Overview',
                  style: theme.textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 8),
                Text(
                  AboutInfo.appDescription,
                  style: theme.textTheme.bodyMedium?.copyWith(
                    height: 1.4,
                  ),
                ),
                const SizedBox(height: 12),
                Text(
                  AboutInfo.deviceDescription,
                  style: theme.textTheme.bodyMedium?.copyWith(
                    height: 1.4,
                  ),
                ),

                const SizedBox(height: 20),

                // Feature Highlights Chips
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    _FeatureChip(
                      icon: Icons.shield_outlined,
                      label: 'NIST P-256 ECDH',
                      colorScheme: colorScheme,
                    ),
                    _FeatureChip(
                      icon: Icons.bluetooth,
                      label: 'BLE Passive Auth',
                      colorScheme: colorScheme,
                    ),
                    _FeatureChip(
                      icon: Icons.cloud_off,
                      label: 'Offline-First',
                      colorScheme: colorScheme,
                    ),
                    _FeatureChip(
                      icon: Icons.memory,
                      label: 'ESP32 Controller',
                      colorScheme: colorScheme,
                    ),
                  ],
                ),

                const SizedBox(height: 20),
                const Divider(),
                const SizedBox(height: 16),

                // System Details Grid
                Text(
                  'Build & Environment',
                  style: theme.textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 12),
                Container(
                  padding: const EdgeInsets.all(14),
                  decoration: BoxDecoration(
                    color: colorScheme.surfaceContainerHighest.withValues(alpha: 0.4),
                    borderRadius: BorderRadius.circular(12),
                    border: Border.all(
                      color: colorScheme.outlineVariant.withValues(alpha: 0.3),
                    ),
                  ),
                  child: Column(
                    children: [
                      _DetailRow(
                        label: 'Developer',
                        value: AboutInfo.developer,
                        theme: theme,
                      ),
                      const SizedBox(height: 8),
                      _DetailRow(
                        label: 'Build Date',
                        value: AboutInfo.buildDate,
                        theme: theme,
                      ),
                      const SizedBox(height: 8),
                      _DetailRow(
                        label: 'Commit Hash',
                        value: AboutInfo.commitHash,
                        theme: theme,
                        isCode: true,
                      ),
                    ],
                  ),
                ),

                const SizedBox(height: 20),

                // Action Buttons Row
                LayoutBuilder(
                  builder: (context, constraints) {
                    final isSmallScreen = constraints.maxWidth < 360;
                    if (isSmallScreen) {
                      return Column(
                        children: [
                          SizedBox(
                            width: double.infinity,
                            child: OutlinedButton.icon(
                              onPressed: () => _showSecurityDialog(context),
                              icon: const Icon(Icons.security, size: 18),
                              label: const Text('Security Specs'),
                            ),
                          ),
                          const SizedBox(height: 8),
                          SizedBox(
                            width: double.infinity,
                            child: OutlinedButton.icon(
                              onPressed: () => _copyDiagnostics(context),
                              icon: const Icon(Icons.copy, size: 18),
                              label: const Text('Copy Diagnostics'),
                            ),
                          ),
                          const SizedBox(height: 8),
                          SizedBox(
                            width: double.infinity,
                            child: OutlinedButton.icon(
                              onPressed: () => _showLicensesDialog(context),
                              icon: const Icon(Icons.description_outlined, size: 18),
                              label: const Text('Open Source Licenses'),
                            ),
                          ),
                        ],
                      );
                    }
                    return Row(
                      children: [
                        Expanded(
                          child: OutlinedButton.icon(
                            onPressed: () => _showSecurityDialog(context),
                            icon: const Icon(Icons.security, size: 16),
                            label: const Text('Security', style: TextStyle(fontSize: 12)),
                          ),
                        ),
                        const SizedBox(width: 6),
                        Expanded(
                          child: OutlinedButton.icon(
                            onPressed: () => _copyDiagnostics(context),
                            icon: const Icon(Icons.copy, size: 16),
                            label: const Text('Diagnostics', style: TextStyle(fontSize: 12)),
                          ),
                        ),
                        const SizedBox(width: 6),
                        Expanded(
                          child: OutlinedButton.icon(
                            onPressed: () => _showLicensesDialog(context),
                            icon: const Icon(Icons.description_outlined, size: 16),
                            label: const Text('Licenses', style: TextStyle(fontSize: 12)),
                          ),
                        ),
                      ],
                    );
                  },
                ),
              ],
            ),
          ),
        ),
      ],
    );
  }
}

class _FeatureChip extends StatelessWidget {
  final IconData icon;
  final String label;
  final ColorScheme colorScheme;

  const _FeatureChip({
    required this.icon,
    required this.label,
    required this.colorScheme,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: colorScheme.secondaryContainer.withValues(alpha: 0.6),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 14, color: colorScheme.onSecondaryContainer),
          const SizedBox(width: 6),
          Text(
            label,
            style: TextStyle(
              fontSize: 12,
              fontWeight: FontWeight.w600,
              color: colorScheme.onSecondaryContainer,
            ),
          ),
        ],
      ),
    );
  }
}

class _DetailRow extends StatelessWidget {
  final String label;
  final String value;
  final ThemeData theme;
  final bool isCode;

  const _DetailRow({
    required this.label,
    required this.value,
    required this.theme,
    this.isCode = false,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = theme.colorScheme;
    return Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: [
        Text(
          label,
          style: theme.textTheme.bodySmall?.copyWith(
            color: colorScheme.onSurfaceVariant,
            fontWeight: FontWeight.w500,
          ),
        ),
        Text(
          value,
          style: theme.textTheme.bodySmall?.copyWith(
            fontWeight: FontWeight.bold,
            fontFamily: isCode ? 'monospace' : null,
            color: colorScheme.onSurface,
          ),
        ),
      ],
    );
  }
}
