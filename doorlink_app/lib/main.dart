import 'dart:async';
import 'about_info.dart';
import 'dart:math';
import 'dart:io' show Platform;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:share_plus/share_plus.dart';
import 'package:receive_sharing_intent/receive_sharing_intent.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

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
  bool _isActive = true;
  String _userId = "";
  String _espPublicKey = "";
  String _appPublicKey = "";
  String _chipId = "";
  late TextEditingController _userIdController;
  late TextEditingController _espPubKeyController;
  late TextEditingController _pubKeyController;
  late TextEditingController _chipIdController;

  // Engine State
  String _status = 'Initializing...';
  String _rangeStatus = 'OUT_OF_RANGE';
  bool _isUnlocked = false;
  Timer? _rangeResetTimer;
  Timer? _unlockResetTimer;
  final List<String> _logs = [];
  final List<Map<String, String>> _pendingUsers = [];
  List<String> _enrolledUsers = [];
  String _lockRelayStatus = "Unknown";
  String _lockSensorStatus = "Unknown";
  bool _isAdminLoading = false;
  
  late StreamSubscription _intentDataStreamSubscription;

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
    _espPubKeyController = TextEditingController(text: _espPublicKey);
    _pubKeyController = TextEditingController(text: _appPublicKey);
    _chipIdController = TextEditingController(text: _chipId);
    _status = 'Loading credentials...';
    platform.setMethodCallHandler(_handleNativeCall);
    _loadOrCreateCredentials().then((_) {
      if (_isActive) _initListening();
      _setupShareIntentListener();
    });
  }

  void _setupShareIntentListener() {
    // 1. Listen for intent payloads shared while the app is already in memory
    _intentDataStreamSubscription = ReceiveSharingIntent.instance.getMediaStream().listen((List<SharedMediaFile> value) {
      if (value.isNotEmpty && value.first.type == SharedMediaType.text) {
        _handleSharedText(value.first.path); // receive_sharing_intent puts text in .path
      } else if (value.isNotEmpty && value.first.type == SharedMediaType.image) {
        _addLog('Received Image intent: ${value.first.path}');
      }
    }, onError: (err) {
      _addLog("Intent stream error: $err");
    });

    // 2. Listen for intent payloads shared when app is completely closed
    ReceiveSharingIntent.instance.getInitialMedia().then((List<SharedMediaFile> value) {
      if (value.isNotEmpty && value.first.type == SharedMediaType.text) {
        _handleSharedText(value.first.path);
      } else if (value.isNotEmpty && value.first.type == SharedMediaType.image) {
        _addLog('Received initial Image intent: ${value.first.path}');
      }
    });
  }

  void _handleSharedText(String text) {
    _addLog('Received shared text:\n$text');
    
    // Regex to extract Fingerprint (User ID) and App Public Key
    final userIdMatch = RegExp(r'Fingerprint \(User ID\):\s*([a-fA-F0-9]+)').firstMatch(text);
    final pubKeyMatch = RegExp(r'App Public Key \(raw hex\):\s*([a-fA-F0-9]+)').firstMatch(text);

    if (userIdMatch != null && pubKeyMatch != null) {
      final parsedUserId = userIdMatch.group(1)!;
      final parsedPubKey = pubKeyMatch.group(1)!;

      _addLog('Successfully parsed User ID: $parsedUserId');
      _addLog('Successfully parsed Public Key.');

      setState(() {
        _pendingUsers.add({
          'userId': parsedUserId,
          'pubKey': parsedPubKey,
        });
        _currentIndex = 1; // Auto-switch to the new Admin tab
      });

      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Parsed user $parsedUserId!'), backgroundColor: Colors.green),
        );
      }
    } else {
      _addLog('Failed to parse User ID or Public Key from shared text.');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Failed to parse DoorLink invite text.'), backgroundColor: Colors.red),
        );
      }
    }

    ReceiveSharingIntent.instance.reset(); // clear the intent so it doesn't trigger again
  }

  void _resetRangeTimer() {
    _rangeResetTimer?.cancel();
    _rangeResetTimer = Timer(const Duration(seconds: 4), () {
      if (mounted && _rangeStatus != 'OUT_OF_RANGE') {
        setState(() => _rangeStatus = 'OUT_OF_RANGE');
      }
    });
  }

  Future<void> _handleNativeCall(MethodCall call) async {
    if (call.method == 'onNativeDebug') {
      final message = call.arguments?.toString() ?? '';
      if (message.isNotEmpty) {
        if (!message.startsWith('RSSI:')) {
          _addLog(message);
        }
        
        if (message.startsWith('RSSI:')) {
          final parts = message.split(':');
          if (parts.length == 2) {
            final rssi = int.tryParse(parts[1]) ?? -100;
            String newStatus = 'FAR';
            if (rssi >= -65) {
               newStatus = 'AT_DOOR';
            } else if (rssi >= -85) {
               newStatus = 'CLOSER';
            }
            if (_rangeStatus != newStatus) {
               setState(() => _rangeStatus = newStatus);
            }
            _resetRangeTimer();
          }
        } else if (message == 'EVENT:CHALLENGE_ROTATED') {
          setState(() {
             _isUnlocked = true;
          });
          _unlockResetTimer?.cancel();
          _unlockResetTimer = Timer(const Duration(seconds: 6), () {
             if (mounted) setState(() => _isUnlocked = false);
          });
        } else if (message.contains('scan started')) {
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
      final storedEspPubKey = stored?['espPubKey']?.toString().trim() ?? '';
      final appPubKey = await platform.invokeMethod<String>('getAppPublicKey') ?? '';
      _addLog('Loaded creds: userId=${storedUserId.isNotEmpty ? storedUserId : "(new)"} pubKeyLen=${appPubKey.length}');

      final nextUserId = storedUserId.isNotEmpty ? storedUserId : _randomHex(4);

      if (storedUserId.isEmpty) {
        await platform.invokeMethod('saveStoredCredentials', {
          'userId': nextUserId,
          'espPubKey': storedEspPubKey,
        });
      }

      if (!mounted) return;
      setState(() {
        _userId = nextUserId;
        _espPublicKey = storedEspPubKey;
        _appPublicKey = appPubKey;
        _userIdController.text = nextUserId;
        _espPubKeyController.text = storedEspPubKey;
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
    _espPubKeyController.dispose();
    _pubKeyController.dispose();
    _chipIdController.dispose();
    _intentDataStreamSubscription.cancel();
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
    await _getAndroidSdkInt();

    final permissionsToRequest = <Permission>[
      Permission.bluetooth,
      Permission.bluetoothAdvertise,
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.notification,
      Permission.location,
    ];

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

    if (_espPublicKey.isEmpty) {
      _addLog("Skipping background service: Missing Lock Public Key (Admin-only mode).");
      if (_isActive) {
        setState(() => _isActive = false);
      }
      return;
    }

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
        'espPubKey': _espPublicKey,
      });
      await platform.invokeMethod('startBackgroundService', {
        'userId': _userId,
        'espPubKey': _espPublicKey,
        'serviceUuid': '0000fcd2-0000-1000-8000-00805f9b34fb',
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

  Future<void> _manualUnlock() async {
    _addLog('Manual unlock requested...');
    
    try {
      await platform.invokeMethod('startBackgroundService', {
        'userId': 'MANUAL_$_userId',
        'espPubKey': _espPublicKey,
        'serviceUuid': '0000fcd3-0000-1000-8000-00805f9b34fb',
      });
      Future.delayed(const Duration(seconds: 5), () {
        if (_isActive && mounted) {
          platform.invokeMethod('startBackgroundService', {
            'userId': _userId,
            'espPubKey': _espPublicKey,
            'serviceUuid': '0000fcd2-0000-1000-8000-00805f9b34fb',
          });
        }
      });
    } catch (e) {
      _addLog('BLE fallback failed: $e');
    }
  }

  Future<void> _enableAdminAp() async {
    _addLog('Enable Admin AP requested...');
    
    try {
      await platform.invokeMethod('enableAdminAp', {
        'userId': _userId,
        'espPubKey': _espPublicKey,
      });
      Future.delayed(const Duration(seconds: 5), () {
        if (_isActive && mounted) {
          platform.invokeMethod('startBackgroundService', {
            'userId': _userId,
            'espPubKey': _espPublicKey,
          });
        }
      });
    } catch (e) {
      _addLog('Failed to trigger AP: $e');
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

  Future<BluetoothDevice> _scanAndConnectToLock() async {
    _addLog('Scanning for live Lock...');
    
    final completer = Completer<BluetoothDevice>();
    final scanStartTime = DateTime.now().subtract(const Duration(seconds: 1));
    
    final subscription = FlutterBluePlus.scanResults.listen((results) {
      for (var r in results) {
        // Ensure we only pick a live advertisement, not a cached one from the background service
        if (r.timeStamp.isAfter(scanStartTime)) {
          if (r.advertisementData.serviceUuids.contains(Guid("0000fcd2-0000-1000-8000-00805f9b34fb"))) {
            if (!completer.isCompleted) {
              completer.complete(r.device);
            }
            break;
          }
        }
      }
    });

    await FlutterBluePlus.startScan(
      withServices: [Guid("0000fcd2-0000-1000-8000-00805f9b34fb")],
      timeout: const Duration(seconds: 5),
    );

    try {
      // Snap the connection the instant the Completer resolves the live MAC
      final targetDevice = await completer.future.timeout(const Duration(seconds: 5));
      await FlutterBluePlus.stopScan();
      subscription.cancel();

      if (Platform.isAndroid) await Future.delayed(const Duration(milliseconds: 300));
      _addLog('Found live lock: ${targetDevice.remoteId}. Connecting...');
      
      try {
        await targetDevice.connect(timeout: const Duration(seconds: 5));
      } catch (e) {
        _addLog('First connect attempt failed. Retrying...');
        await targetDevice.disconnect();
        await Future.delayed(const Duration(milliseconds: 300));
        await targetDevice.connect(timeout: const Duration(seconds: 8));
      }
      return targetDevice;
    } catch (e) {
      await FlutterBluePlus.stopScan();
      subscription.cancel();
      throw Exception('No active DoorLink locks found nearby.');
    }
  }

  Future<void> _fetchPubKeyViaBle() async {
    _addLog('Attempting to fetch Lock Public Key via BLE...');
    setState(() => _status = 'Scanning for lock...');

    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Scanning for DoorLink lock to pair...')),
      );
    }

    bool wasActive = _isActive;
    if (wasActive) {
      _addLog('Pausing background service for reliable BLE GATT...');
      await _stopListening();
      await Future.delayed(const Duration(milliseconds: 1500)); // Let radio and service fully stop
    }

    try {
      final targetDevice = await _scanAndConnectToLock();

      _addLog('Discovering services...');
      final services = await targetDevice.discoverServices();
      
      BluetoothService? adminService;
      for (var s in services) {
        if (s.uuid == Guid("0000fcd4-0000-1000-8000-00805f9b34fb")) {
          adminService = s;
          break;
        }
      }

      if (adminService == null) {
        await targetDevice.disconnect();
        throw Exception('Admin Service (FCD4) not found.');
      }

      BluetoothCharacteristic? pubKeyChar;
      BluetoothCharacteristic? chipIdChar;
      for (var c in adminService.characteristics) {
        if (c.uuid == Guid("0000fcd7-0000-1000-8000-00805f9b34fb")) {
          pubKeyChar = c;
        } else if (c.uuid == Guid("0000fcd6-0000-1000-8000-00805f9b34fb")) {
          chipIdChar = c;
        }
      }

      if (pubKeyChar == null) {
        await targetDevice.disconnect();
        throw Exception('PubKey Characteristic (FCD7) not found.');
      }
      if (chipIdChar == null) {
        await targetDevice.disconnect();
        throw Exception('Chip ID Characteristic (FCD6) not found.');
      }

      _addLog('Reading Public Key and Chip ID via BLE...');
      final value = await pubKeyChar.read();
      final chipValue = await chipIdChar.read();
      final fetchedKey = value.map((e) => e.toRadixString(16).padLeft(2, '0')).join().toUpperCase();
      final fetchedChipId = chipValue.map((e) => e.toRadixString(16).padLeft(2, '0')).join(':').toUpperCase();
      _addLog('Key read successfully.');

      await targetDevice.disconnect();

      if (fetchedKey.isNotEmpty && fetchedKey.length == 130) {
        setState(() {
          _espPublicKey = fetchedKey;
          _espPubKeyController.text = fetchedKey;
          _chipId = fetchedChipId;
          _chipIdController.text = fetchedChipId;
          _status = 'Successfully fetched Lock Public Key';
        });
        _addLog('Successfully paired with lock!');
        await platform.invokeMethod('saveStoredCredentials', {
          'userId': _userId,
          'espPubKey': _espPublicKey,
        });
        
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('Successfully paired with DoorLink lock!'), backgroundColor: Colors.green),
          );
        }
      } else {
        throw Exception('Invalid key received: $fetchedKey');
      }
    } catch (e) {
      setState(() => _status = 'BLE Fetch failed');
      _addLog('BLE Fetch failed: $e');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Failed to pair: $e'), backgroundColor: Colors.red),
        );
      }
    } finally {
      if (wasActive) {
        _addLog('Resuming background service...');
        await _initListening();
      }
    }
  }

  Widget _buildDynamicBanner() {
    IconData icon;
    Color color;
    String title;
    String subtitle;

    if (_isUnlocked) {
      icon = Icons.check_circle;
      color = Colors.green;
      title = 'Door is Unlocked';
      subtitle = 'Welcome!';
    } else if (_rangeStatus == 'OUT_OF_RANGE') {
      icon = Icons.location_off;
      color = Colors.grey;
      title = 'Door is out of range';
      subtitle = 'Walk towards the door.';
    } else if (_rangeStatus == 'AT_DOOR' && _status == 'Broadcasting BLE...') {
      icon = Icons.waving_hand;
      color = Colors.green;
      title = 'Ready to Open';
      subtitle = 'Wave your hand over sensor.';
    } else {
      // FAR or CLOSER
      icon = Icons.directions_walk;
      color = Colors.orange;
      title = 'You are in the range';
      subtitle = 'But be closer to unlock the door.';
    }

    return Container(
      margin: const EdgeInsets.only(top: 20, left: 20, right: 20),
      padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 16),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.1),
        border: Border.all(color: color),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        children: [
          Icon(icon, color: color, size: 28),
          const SizedBox(width: 16),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: TextStyle(
                    color: color == Colors.grey ? Colors.grey.shade800 : (color == Colors.green ? Colors.green : Colors.orange.shade800),
                    fontWeight: FontWeight.bold,
                    fontSize: 16,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  subtitle,
                  style: TextStyle(
                    color: color == Colors.grey ? Colors.grey.shade700 : (color == Colors.green ? Colors.green.shade800 : Colors.orange.shade800),
                    fontSize: 14,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
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
          if (_isActive) _buildDynamicBanner(),
          const SizedBox(height: 40),
          SwitchListTile(
            title: const Text('Enable Auto-Unlock', style: TextStyle(fontWeight: FontWeight.bold)),
            subtitle: const Text('Listen for door beacon in background'),
            value: _isActive,
            activeThumbColor: Colors.green,
            onChanged: _toggleService,
          ),
          const SizedBox(height: 20),
          ElevatedButton.icon(
            onPressed: _isActive ? _manualUnlock : null,
            icon: const Icon(Icons.sensor_door),
            label: const Padding(
              padding: EdgeInsets.all(12.0),
              child: Text('Manual Unlock', style: TextStyle(fontSize: 18)),
            ),
          ),
          const SizedBox(height: 20),
          ElevatedButton.icon(
            onPressed: _isActive ? _enableAdminAp : null,
            icon: const Icon(Icons.wifi_tethering),
            label: const Padding(
              padding: EdgeInsets.all(12.0),
              child: Text('Enable Admin AP (1 Hour)', style: TextStyle(fontSize: 18)),
            ),
            style: ElevatedButton.styleFrom(backgroundColor: Colors.blueGrey),
          ),
          const SizedBox(height: 20),
          ElevatedButton.icon(
            onPressed: _isActive ? _checkDeviceStatusViaBle : null,
            icon: const Icon(Icons.info_outline),
            label: const Padding(
              padding: EdgeInsets.all(12.0),
              child: Text('Check AP Status', style: TextStyle(fontSize: 18)),
            ),
            style: ElevatedButton.styleFrom(backgroundColor: Colors.teal),
          ),
        ],
      ),
    );
  }

  Future<void> _checkDeviceStatusViaBle() async {
    setState(() => _status = 'Checking AP Status...');
    _addLog('Checking device status via BLE...');

    bool wasActive = _isActive;
    if (wasActive) {
      await _stopListening();
      await Future.delayed(const Duration(milliseconds: 1500));
    }

    try {
      final targetDevice = await _scanAndConnectToLock();
      
      _addLog('Discovering services for status check...');
      final services = await targetDevice.discoverServices();
      BluetoothService? adminSvc;
      for (var s in services) {
        if (s.uuid.toString().toUpperCase() == '0000FCD4-0000-1000-8000-00805F9B34FB') {
          adminSvc = s;
          break;
        }
      }

      if (adminSvc == null) {
        throw Exception('Admin Service (0xFCD4) not found.');
      }

      BluetoothCharacteristic? statusChr;
      for (var c in adminSvc.characteristics) {
        if (c.uuid.toString().toUpperCase() == '0000FCDA-0000-1000-8000-00805F9B34FB') {
          statusChr = c;
          break;
        }
      }

      if (statusChr == null) {
        throw Exception('Status Characteristic (0xFCDA) not found.');
      }

      _addLog('Reading status characteristic...');
      final value = await statusChr.read();
      if (value.length >= 4) {
        final apStatus = value[3] == 1 ? 'ON (Enabled)' : 'OFF (Disabled)';
        _addLog('AP Status retrieved: $apStatus');
        
        if (mounted) {
          showDialog(
            context: context,
            builder: (context) => AlertDialog(
              title: const Text('Device Status'),
              content: Text('The Admin Access Point is currently:\n\n$apStatus', style: const TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
              actions: [
                TextButton(
                  onPressed: () => Navigator.pop(context),
                  child: const Text('OK'),
                ),
              ],
            ),
          );
        }
      } else {
        throw Exception('Status payload too small: ${value.length} bytes. Update device firmware.');
      }

      await targetDevice.disconnect();
    } catch (e) {
      _addLog('Status check failed: $e');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Failed to read status: $e')),
        );
      }
    } finally {
      if (wasActive && mounted) {
        _initListening();
      }
      setState(() => _status = 'Idle');
    }
  }

  Future<void> _syncUserViaBle(String userIdHex, String pubKeyHex) async {
    setState(() => _status = 'Admin Sync: Scanning...');
    _addLog('Starting BLE Admin Sync for $userIdHex');

    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Scanning for DoorLink lock...')),
      );
    }

    bool wasActive = _isActive;
    if (wasActive) {
      _addLog('Pausing background service for reliable BLE GATT...');
      await _stopListening();
      await Future.delayed(const Duration(milliseconds: 1500)); // Let radio and service fully stop
    }

    try {
      List<int> hexToBytes(String hex) {
        final clean = hex.trim().toLowerCase();
        return List.generate(clean.length ~/ 2, (i) => int.parse(clean.substring(i * 2, i * 2 + 2), radix: 16));
      }

      final userIdBytes = hexToBytes(userIdHex);
      final pubKeyBytes = hexToBytes(pubKeyHex);
      final payload = [...userIdBytes, ...pubKeyBytes];

      final targetDevice = await _scanAndConnectToLock();

      // 3. Discover Services
      _addLog('Discovering services...');
      final services = await targetDevice.discoverServices();
      
      BluetoothService? adminService;
      for (var s in services) {
        if (s.uuid == Guid("0000fcd4-0000-1000-8000-00805f9b34fb")) {
          adminService = s;
          break;
        }
      }

      if (adminService == null) {
        await targetDevice.disconnect();
        throw Exception('Admin Sync Service (FCD4) not found.');
      }

      BluetoothCharacteristic? syncChar;
      for (var c in adminService.characteristics) {
        if (c.uuid == Guid("0000fcd5-0000-1000-8000-00805f9b34fb")) {
          syncChar = c;
          break;
        }
      }

      if (syncChar == null) {
        await targetDevice.disconnect();
        throw Exception('Admin Sync Characteristic (FCD5) not found.');
      }

      // 4. Write Payload
      _addLog('Writing 69-byte payload...');
      if (Platform.isAndroid) {
         await targetDevice.requestMtu(128); // Ensure we can send all 69 bytes at once
      }
      await syncChar.write(payload, withoutResponse: false);
      _addLog('Payload written successfully!');

      // 5. Disconnect
      await targetDevice.disconnect();

      // 6. Update UI
      setState(() {
        _pendingUsers.removeWhere((u) => u['userId'] == userIdHex);
        _status = 'Admin Sync: Success!';
      });

      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Successfully synced $userIdHex!'), backgroundColor: Colors.green),
        );
      }

    } catch (e) {
      _addLog('Admin Sync failed: $e');
      setState(() => _status = 'Admin Sync: Failed');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Sync failed: $e'), backgroundColor: Colors.red),
        );
      }
    } finally {
      if (wasActive) {
        _addLog('Resuming background service...');
        await _initListening();
      }
    }
  }

  Future<void> _fetchAdminDataViaBle() async {
    setState(() {
      _isAdminLoading = true;
      _status = 'Fetching Lock Data...';
    });
    _addLog('Starting BLE Admin Data Fetch');

    bool wasActive = _isActive;
    if (wasActive) {
      _addLog('Pausing background service for reliable BLE GATT...');
      await _stopListening();
      await Future.delayed(const Duration(milliseconds: 1500));
    }

    try {
      final targetDevice = await _scanAndConnectToLock();
      final services = await targetDevice.discoverServices();
      BluetoothService? adminService;
      for (var s in services) {
        if (s.uuid == Guid("0000fcd4-0000-1000-8000-00805f9b34fb")) adminService = s;
      }
      if (adminService == null) throw Exception('Admin Sync Service (FCD4) not found.');

      BluetoothCharacteristic? usersChar;
      BluetoothCharacteristic? statusChar;
      for (var c in adminService.characteristics) {
        if (c.uuid == Guid("0000fcd8-0000-1000-8000-00805f9b34fb")) usersChar = c;
        if (c.uuid == Guid("0000fcd9-0000-1000-8000-00805f9b34fb")) statusChar = c;
      }

      if (usersChar != null) {
        final usersBytes = await usersChar.read();
        List<String> parsedUsers = [];
        for (int i = 0; i < usersBytes.length; i += 4) {
          if (i + 4 <= usersBytes.length) {
            parsedUsers.add(usersBytes.sublist(i, i + 4).map((e) => e.toRadixString(16).padLeft(2, '0')).join().toUpperCase());
          }
        }
        _enrolledUsers = parsedUsers;
      }

      if (statusChar != null) {
        final statusBytes = await statusChar.read();
        if (statusBytes.length >= 3) {
          _lockRelayStatus = statusBytes[0] == 1 ? "ACTIVE (Unlocked)" : "Locked";
          _lockSensorStatus = statusBytes[1] == 1 ? "DETECTED (Hand present)" : "Clear";
        }
      }

      await targetDevice.disconnect();
      setState(() { _status = 'Admin Data Refreshed'; });

    } catch (e) {
      _addLog('Admin Data Fetch failed: $e');
      setState(() => _status = 'Data Fetch Failed');
    } finally {
      setState(() { _isAdminLoading = false; });
      if (wasActive) await _initListening();
    }
  }

  Future<void> _revokeUserViaBle(String userIdHex) async {
    setState(() => _status = 'Revoking user...');
    bool wasActive = _isActive;
    if (wasActive) {
      await _stopListening();
      await Future.delayed(const Duration(milliseconds: 1500));
    }
    try {
      List<int> hexToBytes(String hex) {
        final clean = hex.trim().toLowerCase();
        return List.generate(clean.length ~/ 2, (i) => int.parse(clean.substring(i * 2, i * 2 + 2), radix: 16));
      }
      final payload = hexToBytes(userIdHex);
      
      final targetDevice = await _scanAndConnectToLock();
      final services = await targetDevice.discoverServices();
      BluetoothCharacteristic? revokeChar;
      for (var s in services) {
        if (s.uuid == Guid("0000fcd4-0000-1000-8000-00805f9b34fb")) {
          for (var c in s.characteristics) {
            if (c.uuid == Guid("0000fcda-0000-1000-8000-00805f9b34fb")) revokeChar = c;
          }
        }
      }

      if (revokeChar == null) throw Exception('Revoke characteristic not found.');
      await revokeChar.write(payload, withoutResponse: false);
      await targetDevice.disconnect();

    } catch (e) {
      _addLog('Revoke failed: $e');
    } finally {
      if (wasActive) await _initListening();
      _fetchAdminDataViaBle(); // Refresh the list automatically
    }
  }

  Widget _buildAdminScreen() {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16.0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            width: double.infinity,
            child: ElevatedButton.icon(
              onPressed: _isAdminLoading ? null : _fetchAdminDataViaBle,
              icon: _isAdminLoading 
                  ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)) 
                  : const Icon(Icons.refresh),
              label: const Padding(padding: EdgeInsets.all(12), child: Text('Connect & Refresh Lock Data')),
            ),
          ),
          const SizedBox(height: 24),
          const Text('System Status', style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
          Card(
            child: ListTile(
              leading: const Icon(Icons.memory, color: Colors.blueGrey, size: 36),
              title: Text('Relay: $_lockRelayStatus'),
              subtitle: Text('Sensor: $_lockSensorStatus'),
            ),
          ),
          const SizedBox(height: 24),
          const Text('Pending Approvals', style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
          if (_pendingUsers.isEmpty) const Text('No pending users. Share a code from WhatsApp.') 
          else ..._pendingUsers.map((user) => Card(
            color: Colors.green.shade50,
            child: ListTile(
              title: Text('User ID: ${user['userId']}'),
              subtitle: const Text('Tap icon to sync to lock via BLE'),
              trailing: IconButton(icon: const Icon(Icons.bluetooth_connected, color: Colors.green), onPressed: () => _syncUserViaBle(user['userId']!, user['pubKey']!)),
            ),
          )),
          const SizedBox(height: 24),
          const Text('Enrolled Users', style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
          if (_enrolledUsers.isEmpty) const Text('No users fetched. Tap refresh to load.') 
          else ..._enrolledUsers.map((uid) => Card(
            child: ListTile(
              leading: const Icon(Icons.person, color: Colors.blueGrey),
              title: Text('User ID: $uid'),
              trailing: IconButton(icon: const Icon(Icons.delete, color: Colors.red), onPressed: () => _revokeUserViaBle(uid)),
            ),
          )),
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
    return SingleChildScrollView(
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
            controller: _espPubKeyController,
            decoration: const InputDecoration(
              labelText: 'Lock Public Key',
              border: OutlineInputBorder(),
              helperText: 'Required to unlock your specific door.',
            ),
            onChanged: (val) {
              _espPublicKey = val;
            },
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _chipIdController,
            readOnly: true,
            decoration: const InputDecoration(
              labelText: 'Lock Hardware Chip ID',
              border: OutlineInputBorder(),
              helperText: 'Provide this to the vendor to generate a license.',
            ),
          ),
          const SizedBox(height: 12),
          SizedBox(
            width: double.infinity,
            child: ElevatedButton.icon(
              onPressed: _fetchPubKeyViaBle,
              icon: const Icon(Icons.bluetooth),
              label: const Text('Pair with Lock (Fetch Key via BLE)'),
            ),
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
                Share.share(
                  'Hello Admin, here is my DoorLink provisioning data to grant me access:\n\nFingerprint (User ID): $_userId\nApp Public Key (raw hex): $_appPublicKey'
                );
              }
            },
            icon: const Icon(Icons.share),
            label: const Text('Share Fingerprint with Admin'),
          ),
        ),
        const SizedBox(height: 32),
        const SizedBox(height: 24),
        const AboutSection(),
        const SizedBox(height: 24),
        const SizedBox(height: 20),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final List<Widget> pages = [
      _buildMainScreen(),
      _buildAdminScreen(),
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
        type: BottomNavigationBarType.fixed,
        selectedItemColor: Colors.green.shade700,
        unselectedItemColor: Colors.grey.shade600,
        onTap: (index) {
          setState(() {
            _currentIndex = index;
          });
        },
        items: const [
          BottomNavigationBarItem(icon: Icon(Icons.home), label: 'Main'),
          BottomNavigationBarItem(icon: Icon(Icons.admin_panel_settings), label: 'Admin'),
          BottomNavigationBarItem(icon: Icon(Icons.monitor_heart), label: 'Diagnostics'),
          BottomNavigationBarItem(icon: Icon(Icons.settings), label: 'Settings'),
        ],
      ),
    );
  }
}
