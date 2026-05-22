import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
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
  String _userId = "4aa4eee6";
  String _hashSeed = "516142cd42f0e5e7";
  late TextEditingController _userIdController;
  late TextEditingController _seedController;

  // Engine State
  String _status = 'Initializing...';
  StreamSubscription<List<ScanResult>>? _scanSubscription;
  bool _isMirroring = false;
  int _devicesFound = 0;
  final Set<String> _seenUuids = {};
  final List<String> _logs = [];

  void _addLog(String msg) {
    print("DEBUG: $msg");
    setState(() {
      _logs.insert(0, "${DateTime.now().toIso8601String().substring(11, 19)}: $msg");
      if (_logs.length > 50) _logs.removeLast();
    });
  }

  @override
  void initState() {
    super.initState();
    _userIdController = TextEditingController(text: _userId);
    _seedController = TextEditingController(text: _hashSeed);
    _status = 'Idle';
  }

  @override
  void dispose() {
    _scanSubscription?.cancel();
    _userIdController.dispose();
    _seedController.dispose();
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
    _scanSubscription?.cancel();
    await FlutterBluePlus.stopScan();
    setState(() {
      _status = 'Service Stopped';
      _isMirroring = false;
      _addLog("Service stopped.");
    });
  }

  Future<void> _initListening() async {
    // Request all necessary system hardware permissions
    await [
      Permission.location,
      Permission.bluetooth,
      Permission.bluetoothAdvertise,
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.nearbyWifiDevices,
    ].request();

    // Wait to ensure the phone's Bluetooth is physically turned ON
    await FlutterBluePlus.adapterState.where((val) => val == BluetoothAdapterState.on).first;

    if (!mounted || !_isActive) return;

    setState(() {
      _status = 'Listening for Beacon...';
      _addLog("Started BLE Scan...");
    });

    // The specific UUID broadcasted by ESP32 (ble_scanner.c)
    final String targetUuid = "9f82c41d-3b7a-4291-a1e6-b5293d0cfa82";
    _seenUuids.clear();

    _scanSubscription?.cancel();
    _scanSubscription = FlutterBluePlus.onScanResults.listen((results) {
      if (!_isActive) return;

      setState(() {
        _devicesFound = results.length; // Visually confirm the scanner is alive
      });

      for (ScanResult r in results) {
        // Log any newly discovered Service UUIDs to the screen
        for (var u in r.advertisementData.serviceUuids) {
          String uuidStr = u.toString().toLowerCase();
          if (!_seenUuids.contains(uuidStr)) {
            _seenUuids.add(uuidStr);
            _addLog("Service UUID: $uuidStr");
          }
        }
        
        // Also log Service Data UUIDs just in case Android categorizes it here
        for (var u in r.advertisementData.serviceData.keys) {
          String uuidStr = u.toString().toLowerCase();
          if (!_seenUuids.contains("data_$uuidStr")) {
            _seenUuids.add("data_$uuidStr");
            _addLog("ServiceData UUID: $uuidStr");
          }
        }

        // String comparison bypasses Guid object reference/case-sensitivity issues
        bool match = r.advertisementData.serviceUuids.any((uuid) => uuid.toString().toLowerCase() == targetUuid);
        if (!match) {
          match = r.advertisementData.serviceData.keys.any((uuid) => uuid.toString().toLowerCase() == targetUuid);
        }
        
        if (match && !_isMirroring) {
          _isMirroring = true;
          _addLog("MATCH! Target UUID found.");
          FlutterBluePlus.stopScan();
          _startMirroring();
        }
      }
    });

    // Scanning without 'withServices' to capture all BLE devices in range
    await FlutterBluePlus.startScan(continuousUpdates: true);
  }

  Future<void> _startMirroring() async {
    setState(() {
      _status = 'Wakeup Beacon detected! Extracting token...';
    });

    try {
      final String statusStr = await platform.invokeMethod('startMirroring', {'userId': _userId});
      if (!mounted) return;

      setState(() {
        if (statusStr == 'SUCCESS') {
          _status = 'Broadcasting BLE...';
          _addLog("BLE Mirror Broadcast Active");
        } else {
          _status = 'Failed: $statusStr';
          _addLog("Mirror Error: $statusStr");
        }
      });

      // Automatically restart listening after 10 seconds to allow for continuous use
      Future.delayed(const Duration(seconds: 10), () {
        if (_isActive && mounted) {
          _isMirroring = false;
          _initListening();
        }
      });
    } on PlatformException catch (e) {
      if (!mounted) return;
      setState(() {
        _status = "Error: '${e.message}'";
      });
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
            activeColor: Colors.green,
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
          Text('BLE devices seen in range: $_devicesFound', style: const TextStyle(fontSize: 16)),
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
            decoration: const InputDecoration(
              labelText: 'Device Fingerprint (User ID)',
              border: OutlineInputBorder(),
              helperText: 'Must match the 8-character hex ID in the Web Console.',
            ),
            onChanged: (val) {
              _userId = val;
            },
          ),
        const SizedBox(height: 20),
          TextField(
            controller: _seedController,
            decoration: const InputDecoration(
              labelText: 'Initial Hash Seed',
              border: OutlineInputBorder(),
              helperText: 'Must match the seed in the Web Console.',
            ),
            onChanged: (val) {
              _hashSeed = val;
            },
          ),
        const SizedBox(height: 20),
        SizedBox(
          width: double.infinity,
          child: ElevatedButton.icon(
            onPressed: () {
              if (_userId.isNotEmpty && _hashSeed.isNotEmpty) {
                Share.share('Hello Admin, here is my DoorLink provisioning data to grant me access:\n\nFingerprint (User ID): $_userId\nInitial Hash Seed: $_hashSeed');
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