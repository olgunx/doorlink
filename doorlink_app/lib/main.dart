import 'dart:async';
import 'dart:math';
import 'dart:io' show Platform;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:share_plus/share_plus.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DoorLink',
      theme: ThemeData(
        primarySwatch: Colors.blueGrey,
        useMaterial3: true,
      ),
      home: const MainAppScreen(),
    );
  }
}

class MainAppScreen extends StatefulWidget {
  const MainAppScreen({super.key});
  @override
  State<MainAppScreen> createState() => _MainAppScreenState();
}

class _MainAppScreenState extends State<MainAppScreen> {
  static const platform = MethodChannel('com.example.blebeacon/lighthouse');
  
  // UI State
  int _currentIndex = 0;
  bool _isActive = false;
  String _userId = "";
  String _appPublicKey = "";
  late TextEditingController _userIdController;
  late TextEditingController _pubKeyController;

  // Engine State
  String _status = 'Initializing...';
  final List<String> _logs = [];

  String _formatTimestamp(DateTime time) {
    final local = time.toLocal();
    final hours = local.hour.toString().padLeft(2, '0');
    final minutes = local.minute.toString().padLeft(2, '0');
    final seconds = local.second.toString().padLeft(2, '0');
    final milliseconds = local.millisecond.toString().padLeft(3, '0');
    return '$hours:$minutes:$seconds.$milliseconds';
  }

  void _addLog(String msg) {
    final timestamp = _formatTimestamp(DateTime.now());
    print("[$timestamp] DEBUG: $msg");
    setState(() {
      _logs.insert(0, "[$timestamp] $msg");
      if (_logs.length > 50) _logs.removeLast();
    });
  }

  @override
  void initState() {
    super.initState();
    _userIdController = TextEditingController(text: _userId);
    _pubKeyController = TextEditingController(text: _appPublicKey);
    _status = 'Loading credentials...';
    platform.setMethodCallHandler(_handleNativeCall);
    _loadOrCreateCredentials();
  }

  Future<void> _handleNativeCall(MethodCall call) async {
    if (call.method == 'onNativeDebug') {
      final message = call.arguments?.toString() ?? '';
      if (message.isNotEmpty) {
        _addLog(message);
        if (message.contains('scan started')) {
          setState(() => _status = 'Waiting for Beacon...');
        } else if (message.contains('Response advertising started')) {
          setState(() => _status = 'Broadcasting BLE...');
        } else if (message.contains('Missing BLE scan permissions')) {
          _addLog('Native service says BLE permissions are missing');
        } else if (message.contains('Missing credentials')) {
          _addLog('Native service did not receive a userId');
        } else if (message.contains('Stop requested')) {
          _addLog('Native service stop requested');
        }
      }
    }
  }

  String _randomHex(int byteCount) {
    final random = Random.secure();
    final bytes = List<int>.generate(byteCount, (_) => random.nextInt(256));
    return bytes.map((byte) => byte.toRadixString(16).padLeft(2, '0')).join();
  }

  Future<void> _loadOrCreateCredentials() async {
    try {
      final stored = await platform.invokeMethod<Map<dynamic, dynamic>>('getStoredCredentials');
      final storedUserId = stored?['userId']?.toString().trim() ?? '';
      final appPubKey = await platform.invokeMethod<String>('getAppPublicKey') ?? '';
      _addLog('Loaded creds: userId=${storedUserId.isNotEmpty ? storedUserId : "(new)"} pubKeyLen=${appPubKey.length}');

      final nextUserId = storedUserId.isNotEmpty ? storedUserId : _randomHex(4);

      if (storedUserId.isEmpty) {
        await platform.invokeMethod('saveStoredCredentials', {
          'userId': nextUserId,
        });
      }

      if (!mounted) return;
      setState(() {
        _userId = nextUserId;
        _appPublicKey = appPubKey;
        _userIdController.text = nextUserId;
        _pubKeyController.text = appPubKey;
        _status = appPubKey.isNotEmpty ? 'Credentials ready' : 'Credentials unavailable yet';
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _status = 'Credential load error';
      });
      _addLog('Credential load error: $e');
    }
  }

  @override
  void dispose() {
    platform.setMethodCallHandler(null);
    _userIdController.dispose();
    _pubKeyController.dispose();
    super.dispose();
  }

  Future<void> _toggleService(bool value) async {
    setState(() {
      _isActive = value;
    });

    if (_isActive) {
      _initListening();
    } else {
      _stopListening();
    }
  }

  Future<void> _stopListening() async {
    try {
      _addLog('Requesting foreground service stop');
      await platform.invokeMethod('stopBackgroundService');
    } catch (_) {}
    setState(() {
      _status = 'Service Stopped';
      _addLog("Service stopped.");
    });
  }

  Future<void> _initListening() async {
    final sdkInt = await _getAndroidSdkInt();

    final permissionsToRequest = <Permission>[
      Permission.bluetooth,
      Permission.bluetoothAdvertise,
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.notification,
    ];
    if (!Platform.isAndroid || sdkInt < 31) {
      permissionsToRequest.add(Permission.location);
    }

    // Request all necessary system hardware permissions
    Map<Permission, PermissionStatus> statuses = await permissionsToRequest.request();
    _addLog("Permission results: ${statuses.entries.map((e) => '${e.key.toString().split('.').last}=${e.value.isGranted ? 'granted' : 'denied'}').join(', ')}");

    List<String> deniedPerms = [];
    statuses.forEach((perm, status) {
      if (!status.isGranted) {
        deniedPerms.add(perm.toString().split('.').last);
      }
    });

    if (deniedPerms.isNotEmpty) {
      setState(() => _status = 'Permission Denied! Check AndroidManifest.xml');
      _addLog("Error: Denied: ${deniedPerms.join(', ')}");
      return;
    }

    if (!mounted || !_isActive) return;

    setState(() {
      _status = 'Starting background service...';
      _addLog("SC[X]BG[ ]CH[ ]AD[ ]OK[ ]");
    });
    _addLog('Starting foreground service with userId=$_userId pubKeyLen=${_appPublicKey.length}');
    final batteryIgnored = await _isIgnoringBatteryOptimizations();
    _addLog('Battery optimization ignored=${batteryIgnored ? "yes" : "no"}');

    try {
      await platform.invokeMethod('saveStoredCredentials', {
        'userId': _userId,
      });
      await platform.invokeMethod('startBackgroundService', {
        'userId': _userId,
      });
      if (!mounted || !_isActive) return;
      setState(() {
        _status = 'Waiting for Beacon...';
        _addLog("SC[X]BG[X]CH[ ]AD[ ]OK[ ]");
      });
      _addLog('Foreground service start requested successfully');
    } on PlatformException catch (e) {
      if (!mounted) return;
      setState(() {
        _status = "Error: '${e.message}'";
      });
      _addLog("Background start failed: ${e.message ?? 'unknown'}");
    }
  }

  Future<int> _getAndroidSdkInt() async {
    if (!Platform.isAndroid) return 0;
    try {
      final value = await platform.invokeMethod<int>('getAndroidSdkInt');
      return value ?? 0;
    } on PlatformException {
      return 0;
    }
  }

  Future<void> _openBatteryOptimizationSettings() async {
    try {
      await platform.invokeMethod('openBatteryOptimizationSettings');
      _addLog('Opened battery optimization settings');
    } catch (e) {
      _addLog('Failed to open battery settings: $e');
    }
  }

  Future<bool> _isIgnoringBatteryOptimizations() async {
    if (!Platform.isAndroid) return false;
    try {
      final value = await platform.invokeMethod<bool>('isIgnoringBatteryOptimizations');
      return value ?? false;
    } on PlatformException {
      return false;
    }
  }

  Widget _buildMainScreen() {
    return Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(
            _isActive ? Icons.lock_open : Icons.lock,
            size: 100,
            color: _isActive ? Colors.green : Colors.red,
          ),
          const SizedBox(height: 20),
          Text(
            _isActive ? 'DoorLink is ACTIVE' : 'DoorLink is OFF',
            style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold),
          ),
          const SizedBox(height: 10),
          Text('Status: $_status', style: const TextStyle(fontSize: 16, color: Colors.grey)),
          const SizedBox(height: 60),
          SwitchListTile(
            title: const Text('Enable Auto-Unlock', style: TextStyle(fontWeight: FontWeight.bold)),
            subtitle: const Text('Listen for door beacon in background'),
            value: _isActive,
            activeThumbColor: Colors.green,
            onChanged: _toggleService,
          ),
        ],
      ),
    );
  }

  Widget _buildDiagnosticsScreen() {
    return Padding(
      padding: const EdgeInsets.all(16.0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('Native log entries: ${_logs.length}', style: const TextStyle(fontSize: 16)),
          const SizedBox(height: 10),
          const Text('Raw Event Logs:', style: TextStyle(fontWeight: FontWeight.bold)),
          const SizedBox(height: 10),
          Expanded(
            child: Container(
              color: Colors.black87,
              width: double.infinity,
              child: ListView.builder(
                itemCount: _logs.length,
                itemBuilder: (context, index) {
                  return Padding(
                    padding: const EdgeInsets.symmetric(vertical: 2.0, horizontal: 8.0),
                    child: Text(_logs[index], style: const TextStyle(fontSize: 12, fontFamily: 'monospace', color: Colors.greenAccent)),
                  );
                },
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildSettingsScreen() {
    return Padding(
      padding: const EdgeInsets.all(16.0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('Configuration', style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold)),
          const SizedBox(height: 20),
          TextField(
            controller: _userIdController,
            readOnly: true,
            decoration: const InputDecoration(
              labelText: 'Device Fingerprint (User ID)',
              border: OutlineInputBorder(),
              helperText: 'Generated per phone; keep it unique.',
            ),
            onChanged: (val) {
              _userId = val;
            },
          ),
        const SizedBox(height: 20),
          TextField(
            controller: _pubKeyController,
            readOnly: true,
            decoration: const InputDecoration(
              labelText: 'App Public Key',
              border: OutlineInputBorder(),
              helperText: 'Share this with the admin via messaging; it is public.',
            ),
            onChanged: (val) {
              _appPublicKey = val;
            },
          ),
        const SizedBox(height: 20),
        SizedBox(
          width: double.infinity,
          child: OutlinedButton.icon(
            onPressed: _loadOrCreateCredentials,
            icon: const Icon(Icons.refresh),
            label: const Text('Refresh Credentials'),
          ),
        ),
        const SizedBox(height: 12),
        SizedBox(
          width: double.infinity,
          child: OutlinedButton.icon(
            onPressed: _openBatteryOptimizationSettings,
            icon: const Icon(Icons.battery_alert_outlined),
            label: const Text('Open Battery Settings'),
          ),
        ),
        const SizedBox(height: 20),
        SizedBox(
          width: double.infinity,
            child: ElevatedButton.icon(
            onPressed: () {
              if (_userId.isNotEmpty && _appPublicKey.isNotEmpty) {
                SharePlus.instance.share(
                  ShareParams(
                    text: 'Hello Admin, here is my DoorLink provisioning data to grant me access:\n\nFingerprint (User ID): $_userId\nApp Public Key (raw hex): $_appPublicKey',
                  ),
                );
              }
            },
            icon: const Icon(Icons.share),
            label: const Text('Share Fingerprint with Admin'),
          ),
        ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final List<Widget> pages = [
      _buildMainScreen(),
      _buildDiagnosticsScreen(),
      _buildSettingsScreen(),
    ];

    return Scaffold(
      appBar: AppBar(
        title: const Text('DoorLink', style: TextStyle(color: Colors.white)),
        backgroundColor: _isActive ? Colors.green.shade700 : Colors.blueGrey,
      ),
      body: pages[_currentIndex],
      bottomNavigationBar: BottomNavigationBar(
        currentIndex: _currentIndex,
        onTap: (index) {
          setState(() {
            _currentIndex = index;
          });
        },
        items: const [
          BottomNavigationBarItem(icon: Icon(Icons.home), label: 'Main'),
          BottomNavigationBarItem(icon: Icon(Icons.monitor_heart), label: 'Diagnostics'),
          BottomNavigationBarItem(icon: Icon(Icons.settings), label: 'Settings'),
        ],
      ),
    );
  }
}
