// lib/main.dart
// Flutter pump controller app
// Reads pump/status from Firebase, writes pump/cmd

import 'dart:async';
import 'package:flutter/material.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_database/firebase_database.dart';
import 'package:firebase_messaging/firebase_messaging.dart';
import 'logs_page.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:url_launcher/url_launcher.dart';
import 'auth_screen.dart';

// ─── FCM background handler (must be top-level) ───────────────────────────────
@pragma('vm:entry-point')
Future<void> _fcmBackgroundHandler(RemoteMessage message) async {
  await Firebase.initializeApp(
    options: const FirebaseOptions(
      apiKey:            "AIzaSyAKRv98QPE4FraRgGypvdvJfCG0RQs97O0",
      appId:             "1:22800348697:android:89f6c77fdb6492a797dc88",
      messagingSenderId: "22800348697",
      projectId:         "pump-controller-4398d",
      databaseURL:       "https://pump-controller-4398d-default-rtdb.firebaseio.com",
    ),
  );
  // Background messages are shown automatically by FCM on Android.
}

// ─── Local notifications plugin instance ─────────────────────────────────────
final FlutterLocalNotificationsPlugin _localNotifications =
    FlutterLocalNotificationsPlugin();

const AndroidNotificationChannel _alertChannel = AndroidNotificationChannel(
  'pump_alerts',
  'Pump Alerts',
  description: 'Protection trips, relay events, and device status',
  importance: Importance.high,
);

// ─── Site configuration ───────────────────────────────────────────────────────
class SiteConfig {
  final String id;
  final String name;
  final String meterPumpId; // pump whose status feeds PowerMeterCard (legacy)
  final String deviceId;    // physical STM32 device (legacy, used by Settings)
  final List<String> pumpIds; // index 0 → relay1, index 1 → relay2 on deviceId
  // New Firebase hierarchy: sites/{siteId}/line{NN}/pump{NN}/
  final List<String> pumpFbPaths;  // Firebase base path per pump, same order as pumpIds
  final String masterFbBase;       // Firebase base for master device (line01/pump01)
  final String rotationFbPath;     // Firebase path for rotation_schedule doc
  final String simPhone;           // SIM phone number of the EC200U modem (for SMS reset)
  // Slave (Line 2) is activated dynamically via sites/{siteId}/config/slave_fb_path

  const SiteConfig({
    required this.id,
    required this.name,
    required this.meterPumpId,
    required this.deviceId,
    required this.pumpIds,
    required this.pumpFbPaths,
    required this.masterFbBase,
    required this.rotationFbPath,
    required this.simPhone,
  });
}

const kSites = [
  SiteConfig(
    id: 'site01',
    name: 'Site 1',
    meterPumpId: 'pump01',
    deviceId: 'pump01',
    pumpIds: ['pump01', 'pump02'],
    pumpFbPaths: [
      'sites/site01/line01/pump01',
      'sites/site01/line01/pump02',
    ],
    masterFbBase:   'sites/site01/line01/pump01',
    rotationFbPath: 'sites/site01/line01/rotation_schedule',
    simPhone:       '7418596874',
  ),
  SiteConfig(
    id: 'site02',
    name: 'Site 2',
    meterPumpId: 'pump03',
    deviceId: 'pump03',
    pumpIds: ['pump03', 'pump04'],
    pumpFbPaths: [
      'sites/site02/line01/pump01',
      'sites/site02/line01/pump02',
    ],
    masterFbBase:   'sites/site02/line01/pump01',
    rotationFbPath: 'sites/site02/line01/rotation_schedule',
    simPhone:       '9787440880',
  ),
];

// FCM topic derived from Firebase base path — matches bridge.js fcmTopic().
// e.g. 'sites/site01/line01/pump01' → 'site01_line01_pump01'
String fcmTopicFromFbPath(String fbBase) => fbBase
    .replaceFirst('sites/', '')
    .replaceAll('/', '_');

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp(
    options: const FirebaseOptions(
      apiKey:            "AIzaSyAKRv98QPE4FraRgGypvdvJfCG0RQs97O0",
      appId:             "1:22800348697:android:89f6c77fdb6492a797dc88",
      messagingSenderId: "22800348697",
      projectId:         "pump-controller-4398d",
      databaseURL:       "https://pump-controller-4398d-default-rtdb.firebaseio.com",
    ),
  );

  // Register FCM background handler before app starts
  FirebaseMessaging.onBackgroundMessage(_fcmBackgroundHandler);

  // Create the Android notification channel for local (foreground) notifications
  await _localNotifications
      .resolvePlatformSpecificImplementation<
          AndroidFlutterLocalNotificationsPlugin>()
      ?.createNotificationChannel(_alertChannel);

  await _localNotifications.initialize(
    settings: const InitializationSettings(
      android: AndroidInitializationSettings('@mipmap/ic_launcher'),
    ),
  );

  runApp(const PumpApp());
}

class PumpApp extends StatelessWidget {
  const PumpApp({super.key});
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Pump Controller',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.blue),
        useMaterial3: true,
      ),
      home: AuthGate(
        dashboardBuilder: (siteIds) => PumpDashboard(allowedSiteIds: siteIds),
      ),
    );
  }
}

// ─── Dashboard — mutual exclusion coordinator ─────────────────────────────────
class PumpDashboard extends StatefulWidget {
  final List<String> allowedSiteIds;
  const PumpDashboard({super.key, required this.allowedSiteIds});
  @override
  State<PumpDashboard> createState() => _PumpDashboardState();
}

class _PumpDashboardState extends State<PumpDashboard> {
  final db = FirebaseDatabase.instance;

  late final List<SiteConfig> _sites;
  final Map<String, bool?> _pumpOn = {};
  bool _showRotation = false;

  // Dynamic slave config — loaded from sites/{siteId}/config/slave_fb_path
  final Map<String, String?> _slavePaths  = {};          // siteId → slaveFbPath or null
  final Map<String, String?> _simNumbers  = {};          // siteId → SIM number for SMS reset
  final List<StreamSubscription<DatabaseEvent>> _configSubs = [];
  final Map<String, String?> _subscribedSlaveTopics = {}; // siteId → currently-subscribed FCM topic

  @override
  void initState() {
    super.initState();
    _sites = kSites.where((s) => widget.allowedSiteIds.contains(s.id)).toList();
    for (final site in _sites) {
      for (var i = 0; i < site.pumpIds.length; i++) {
        final pumpId = site.pumpIds[i];
        db.ref('${site.pumpFbPaths[i]}/status/relay1_state').onValue.listen((event) {
          if (mounted) setState(() => _pumpOn[pumpId] = (event.snapshot.value ?? 0) == 1);
        });
      }
      // Default to null until cache/Firebase responds
      _slavePaths[site.id] = null;
    }
    _loadCachedSlavePaths();   // instant — avoids startup flicker on subsequent launches
    _subscribeSlaveConfigs();  // live Firebase updates
    _initFCM();
    _loadShowRotation();
  }

  @override
  void dispose() {
    for (final sub in _configSubs) {
      sub.cancel();
    }
    super.dispose();
  }

  /// Load slave paths saved from the previous session so the UI shows correctly
  /// before the Firebase response arrives (no flicker, works offline).
  Future<void> _loadCachedSlavePaths() async {
    final prefs = await SharedPreferences.getInstance();
    if (!mounted) return;
    setState(() {
      for (final site in _sites) {
        _slavePaths[site.id] = prefs.getString('slave_path_${site.id}');
      }
    });
  }

  Future<void> _cacheSlavePath(String siteId, String? path) async {
    final prefs = await SharedPreferences.getInstance();
    if (path != null) {
      await prefs.setString('slave_path_$siteId', path);
    } else {
      await prefs.remove('slave_path_$siteId');
    }
  }

  /// Subscribe to sites/{siteId}/config for each site. When slave_fb_path changes,
  /// update the UI and re-manage the FCM topic subscription.
  void _subscribeSlaveConfigs() {
    for (final site in _sites) {
      final sub = db.ref('sites/${site.id}/config').onValue.listen((event) {
        if (!mounted) return;
        String? slavePath;
        String? simNumber;
        final data = event.snapshot.value;
        if (data is Map) {
          final v = data['slave_fb_path'];
          if (v is String && v.isNotEmpty) slavePath = v;
          final s = data['sim_number'];
          if (s is String && s.isNotEmpty) simNumber = s;
        }
        if (_slavePaths[site.id] != slavePath) {
          setState(() => _slavePaths[site.id] = slavePath);
          _cacheSlavePath(site.id, slavePath);
          _updateSlaveFcmTopic(site.id, slavePath);
        }
        if (_simNumbers[site.id] != simNumber) {
          setState(() => _simNumbers[site.id] = simNumber);
        }
      });
      _configSubs.add(sub);
    }
  }

  /// Subscribe or unsubscribe from the slave FCM topic when the slave path changes.
  Future<void> _updateSlaveFcmTopic(String siteId, String? newPath) async {
    final oldPath = _subscribedSlaveTopics[siteId];
    if (oldPath == newPath) return;
    final messaging = FirebaseMessaging.instance;
    if (oldPath != null) {
      await messaging.unsubscribeFromTopic(fcmTopicFromFbPath(oldPath));
    }
    if (newPath != null) {
      await messaging.subscribeToTopic(fcmTopicFromFbPath(newPath));
    }
    _subscribedSlaveTopics[siteId] = newPath;
  }

  Future<void> _loadShowRotation() async {
    final prefs = await SharedPreferences.getInstance();
    if (mounted) setState(() => _showRotation = prefs.getBool('show_rotation_card') ?? false);
  }

  Future<void> _initFCM() async {
    final messaging = FirebaseMessaging.instance;

    // Request permission (Android 13+ / iOS)
    await messaging.requestPermission(alert: true, badge: true, sound: true);

    // Subscribe to FCM topics for master pumps (slave topics managed dynamically
    // by _updateSlaveFcmTopic when sites/{siteId}/config/slave_fb_path changes)
    for (final site in _sites) {
      for (final fbBase in site.pumpFbPaths) {
        await messaging.subscribeToTopic(fcmTopicFromFbPath(fbBase));
      }
    }

    // Show notification when app is in foreground
    FirebaseMessaging.onMessage.listen((RemoteMessage message) {
      final n = message.notification;
      if (n == null) return;
      _localNotifications.show(
        id: message.hashCode,
        title: n.title,
        body: n.body,
        notificationDetails: NotificationDetails(
          android: AndroidNotificationDetails(
            _alertChannel.id,
            _alertChannel.name,
            channelDescription: _alertChannel.description,
            importance: Importance.high,
            priority: Priority.high,
            icon: '@mipmap/ic_launcher',
          ),
        ),
      );
    });
  }

  Future<void> _handlePumpToggle(String pumpId, bool turnOn) async {
    final site   = kSites.firstWhere((s) => s.pumpIds.contains(pumpId), orElse: () => kSites.first);
    final fbBase = site.pumpFbPaths[site.pumpIds.indexOf(pumpId)];
    await db.ref('$fbBase/cmd').set({
      'relay1': turnOn ? 1 : 0,
      'ts': DateTime.now().millisecondsSinceEpoch,
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Pump Controller'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        actions: [
          IconButton(
            icon: const Icon(Icons.history),
            tooltip: 'Logs',
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute(builder: (_) => LogsPage(
                pumpIds:     _sites.expand((s) => s.pumpIds).toList(),
                pumpFbBases: _sites.expand((s) => s.pumpFbPaths).toList(),
                slaveFbPath: _slavePaths.values
                    .firstWhere((v) => v != null, orElse: () => null),
              )),
            ),
          ),
          IconButton(
            icon: const Icon(Icons.settings),
            tooltip: 'Protection Settings',
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute(builder: (_) => SettingsPage(
                pumpIds:     _sites.expand((s) => s.pumpIds).toList(),
                slaveFbPath: _slavePaths.values
                    .firstWhere((v) => v != null, orElse: () => null),
              )),
            ).then((_) => _loadShowRotation()),
          ),
          IconButton(
            icon: const Icon(Icons.logout),
            tooltip: 'Sign out',
            onPressed: () => FirebaseAuth.instance.signOut(),
          ),
        ],
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            for (final site in _sites)
              _SiteSection(
                site: site,
                pumpOn: _pumpOn,
                onPumpToggle: _handlePumpToggle,
                showHeader: _sites.length > 1,
                showRotation: _showRotation,
                slaveFbPath: _slavePaths[site.id],
                simNumber:   _simNumbers[site.id],
              ),
          ],
        ),
      ),
    );
  }
}

// ─── Site section — groups PowerMeter + Pumps + Rotation for one site ─────────
class _SiteSection extends StatefulWidget {
  final SiteConfig site;
  final Map<String, bool?> pumpOn;
  final Future<void> Function(String pumpId, bool on) onPumpToggle;
  final bool showHeader;
  final bool showRotation;
  final String? slaveFbPath; // dynamic — from sites/{siteId}/config/slave_fb_path
  final String? simNumber;   // SIM number for SMS reset — from sites/{siteId}/config/sim_number

  const _SiteSection({
    required this.site,
    required this.pumpOn,
    required this.onPumpToggle,
    required this.showHeader,
    required this.showRotation,
    this.slaveFbPath,
    this.simNumber,
  });

  @override
  State<_SiteSection> createState() => _SiteSectionState();
}

class _SiteSectionState extends State<_SiteSection> {
  int _lineIndex = 0;
  bool _goingForward = true;
  double _dragStartX = 0;
  double _dragCurrentX = 0;

  void _goTo(int idx) {
    if (idx == _lineIndex) return;
    setState(() {
      _goingForward = idx > _lineIndex;
      _lineIndex = idx;
    });
  }

  @override
  Widget build(BuildContext context) {
    final bool hasTwoLines = widget.slaveFbPath != null;
    final ColorScheme colors = Theme.of(context).colorScheme;

    Widget content = AnimatedSwitcher(
      duration: const Duration(milliseconds: 280),
      transitionBuilder: (child, animation) {
        final isNewChild = child.key == ValueKey('line$_lineIndex');
        final offsetTween = isNewChild
            ? Tween<Offset>(
                begin: Offset(_goingForward ? 1.0 : -1.0, 0),
                end: Offset.zero)
            : Tween<Offset>(
                begin: Offset.zero,
                end: Offset(_goingForward ? -1.0 : 1.0, 0));
        return ClipRect(
          child: SlideTransition(
            position: offsetTween.animate(
                CurvedAnimation(parent: animation, curve: Curves.easeInOut)),
            child: child,
          ),
        );
      },
      child: _lineIndex == 0 ? _buildLine1() : _buildLine2(),
    );

    if (hasTwoLines) {
      content = GestureDetector(
        behavior: HitTestBehavior.translucent,
        onHorizontalDragStart: (d) {
          _dragStartX = d.globalPosition.dx;
          _dragCurrentX = d.globalPosition.dx;
        },
        onHorizontalDragUpdate: (d) {
          _dragCurrentX = d.globalPosition.dx;
        },
        onHorizontalDragEnd: (d) {
          final dx = _dragCurrentX - _dragStartX;
          final vx = d.velocity.pixelsPerSecond.dx;
          if (dx < -50 || vx < -500) _goTo(1);
          if (dx > 50 || vx > 500) _goTo(0);
        },
        child: content,
      );
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        if (widget.showHeader || hasTwoLines)
          Padding(
            padding: const EdgeInsets.fromLTRB(4, 8, 4, 8),
            child: Row(
              children: [
                if (widget.showHeader)
                  Text(
                    widget.site.name,
                    style: Theme.of(context)
                        .textTheme
                        .titleMedium
                        ?.copyWith(fontWeight: FontWeight.bold),
                  ),
                if (widget.showHeader && hasTwoLines) const Spacer(),
                if (hasTwoLines)
                  _LinePills(
                    count: 2,
                    current: _lineIndex,
                    onTap: _goTo,
                    colors: colors,
                  ),
              ],
            ),
          ),
        content,
      ],
    );
  }

  Widget _buildLine1() {
    return Column(
      key: const ValueKey('line0'),
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        PowerMeterCard(fbPath: widget.site.masterFbBase, simPhone: widget.site.simPhone),
        const SizedBox(height: 16),
        for (var i = 0; i < widget.site.pumpIds.length; i++) ...[
          PumpCard(
            pumpId: widget.site.pumpIds[i],
            pumpName: 'Pump ${i + 1}',
            fbBase:       widget.site.pumpFbPaths[i],
            statusFbBase: widget.site.pumpFbPaths[i],
            otherPumpOn: widget.site.pumpIds
                .where((p) => p != widget.site.pumpIds[i])
                .any((p) => widget.pumpOn[p] == true),
            otherPumpName: 'Pump ${i == 0 ? 2 : 1}',
            onPumpToggle: (val) =>
                widget.onPumpToggle(widget.site.pumpIds[i], val),
            showSchedule: widget.showRotation,
            masterStatusFbBase: widget.site.masterFbBase,
          ),
          const SizedBox(height: 16),
        ],
        RotationScheduleCard(
          rotationFbPath: widget.site.rotationFbPath,
          pump1Id: widget.site.pumpIds[0],
          pump2Id: widget.site.pumpIds[1],
        ),
        if (widget.simNumber != null) ...[
          const SizedBox(height: 12),
          _buildResetButton(),
        ],
      ],
    );
  }

  Widget _buildResetButton() {
    return SizedBox(
      width: double.infinity,
      child: OutlinedButton.icon(
        icon: const Icon(Icons.restart_alt, color: Colors.red),
        label: const Text(
          'Reset Controller',
          style: TextStyle(color: Colors.red),
        ),
        style: OutlinedButton.styleFrom(
          side: const BorderSide(color: Colors.red),
          padding: const EdgeInsets.symmetric(vertical: 12),
        ),
        onPressed: _confirmAndReset,
      ),
    );
  }

  Future<void> _confirmAndReset() async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Reset Controller?'),
        content: const Text(
          'This sends an SMS command to restart the STM32 board.\n\n'
          'Relay states are saved and will be restored after boot. '
          'The board will be offline for ~30 seconds.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () => Navigator.pop(ctx, true),
            style: TextButton.styleFrom(foregroundColor: Colors.red),
            child: const Text('Send Reset SMS'),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;

    final uri = Uri.parse('sms:${widget.simNumber}?body=RESET');
    await launchUrl(uri, mode: LaunchMode.externalApplication);
  }

  Widget _buildLine2() {
    return Column(
      key: const ValueKey('line1'),
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        if (widget.slaveFbPath != null)
          SlaveStatusCard(slaveFbPath: widget.slaveFbPath!),
      ],
    );
  }
}

// ─── Line pill selector ────────────────────────────────────────────────────────
class _LinePills extends StatelessWidget {
  final int count;
  final int current;
  final ValueChanged<int> onTap;
  final ColorScheme colors;

  const _LinePills({
    required this.count,
    required this.current,
    required this.onTap,
    required this.colors,
  });

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: List.generate(count, (i) {
        final bool active = i == current;
        return GestureDetector(
          onTap: () => onTap(i),
          child: AnimatedContainer(
            duration: const Duration(milliseconds: 200),
            margin: EdgeInsets.only(left: i > 0 ? 6 : 0),
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
            decoration: BoxDecoration(
              color: active ? colors.primary : Colors.grey.shade200,
              borderRadius: BorderRadius.circular(20),
            ),
            child: Text(
              'Line ${i + 1}',
              style: TextStyle(
                fontSize: 12,
                fontWeight: active ? FontWeight.bold : FontWeight.normal,
                color: active ? colors.onPrimary : Colors.grey.shade600,
              ),
            ),
          ),
        );
      }),
    );
  }
}

// ─── Rotation schedule card ───────────────────────────────────────────────────
class RotationScheduleCard extends StatefulWidget {
  final String rotationFbPath; // Firebase path for rotation_schedule doc
  final String pump1Id;
  final String pump2Id;

  const RotationScheduleCard({
    super.key,
    required this.rotationFbPath,
    required this.pump1Id,
    required this.pump2Id,
  });
  @override
  State<RotationScheduleCard> createState() => _RotationScheduleCardState();
}

class _RotationScheduleCardState extends State<RotationScheduleCard> {
  final db = FirebaseDatabase.instance;

  bool   _enabled         = false;
  int    _intervalMinutes = 240; // default 4 h
  late String _currentPump;
  int    _startedAt       = 0;
  bool   _expanded        = false;
  Timer? _ticker;

  static const _options = [
    (label: '30 min',  minutes: 30),
    (label: '1 hour',  minutes: 60),
    (label: '2 hours', minutes: 120),
    (label: '3 hours', minutes: 180),
    (label: '4 hours', minutes: 240),
    (label: '6 hours', minutes: 360),
    (label: '8 hours', minutes: 480),
    (label: '12 hours',minutes: 720),
  ];

  @override
  void initState() {
    super.initState();
    _currentPump = widget.pump1Id;
    db.ref(widget.rotationFbPath).onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        final s = Map<String, dynamic>.from(data as Map);
        setState(() {
          _enabled         = s['enabled']          ?? false;
          _intervalMinutes = (s['interval_minutes'] ?? 240) as int;
          _currentPump     = s['current_pump']      ?? widget.pump1Id;
          _startedAt       = (s['started_at']       ?? 0) as int;
        });
      }
    });
    // Refresh remaining time display every minute
    _ticker = Timer.periodic(const Duration(minutes: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _ticker?.cancel();
    super.dispose();
  }

  String _timeRemaining() {
    if (!_enabled || _startedAt == 0) return '';
    final elapsedMs  = DateTime.now().millisecondsSinceEpoch - _startedAt;
    final intervalMs = _intervalMinutes * 60 * 1000;
    final remainMs   = intervalMs - elapsedMs;
    if (remainMs <= 0) return 'Switching soon...';
    final h = remainMs ~/ 3600000;
    final m = (remainMs % 3600000) ~/ 60000;
    return h > 0 ? '${h}h ${m}m remaining' : '${m}m remaining';
  }

  Future<void> _save() async {
    await db.ref(widget.rotationFbPath).update({
      'enabled':          _enabled,
      'interval_minutes': _intervalMinutes,
      'started_at':       0,   // always reset so bridge re-initializes the timer
    });
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Rotation schedule saved'),
            duration: Duration(seconds: 2)));
    }
  }

  @override
  Widget build(BuildContext context) {
    final activePumpLabel = _currentPump == widget.pump1Id ? 'Pump 1' : 'Pump 2';
    final remaining       = _timeRemaining();

    return Card(
      elevation: 2,
      color: Colors.teal.shade50,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            // ── Header row ──────────────────────────────────────────────────
            InkWell(
              onTap: () => setState(() => _expanded = !_expanded),
              child: Row(
                children: [
                  const Icon(Icons.autorenew, color: Colors.teal),
                  const SizedBox(width: 8),
                  const Text('Pump Rotation',
                      style: TextStyle(
                          fontSize: 16,
                          fontWeight: FontWeight.bold,
                          color: Colors.teal)),
                  const Spacer(),
                  if (_enabled) ...[
                    Container(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 8, vertical: 2),
                      decoration: BoxDecoration(
                        color: Colors.teal,
                        borderRadius: BorderRadius.circular(10),
                      ),
                      child: Text('$activePumpLabel active',
                          style: const TextStyle(
                              color: Colors.white, fontSize: 11)),
                    ),
                    const SizedBox(width: 6),
                  ],
                  Icon(_expanded ? Icons.expand_less : Icons.expand_more,
                      color: Colors.teal),
                ],
              ),
            ),

            if (_enabled && remaining.isNotEmpty) ...[
              const SizedBox(height: 4),
              Text(remaining,
                  style: TextStyle(fontSize: 12, color: Colors.teal.shade700)),
            ],

            if (_expanded) ...[
              const SizedBox(height: 12),
              // Enable toggle
              Row(
                children: [
                  const Text('Enable rotation'),
                  const Spacer(),
                  Switch(
                    value: _enabled,
                    activeThumbColor: Colors.teal,
                    onChanged: (v) => setState(() => _enabled = v),
                  ),
                ],
              ),
              // Interval picker
              Row(
                children: [
                  const Text('Switch every'),
                  const Spacer(),
                  DropdownButton<int>(
                    value: _intervalMinutes,
                    items: _options
                        .map((o) => DropdownMenuItem(
                              value: o.minutes,
                              child: Text(o.label),
                            ))
                        .toList(),
                    onChanged: (v) =>
                        setState(() => _intervalMinutes = v ?? 240),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              SizedBox(
                width: double.infinity,
                child: ElevatedButton.icon(
                  icon: const Icon(Icons.save, size: 16),
                  label: const Text('Save Rotation'),
                  style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.teal,
                      foregroundColor: Colors.white),
                  onPressed: _save,
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

// ─── Individual pump card ─────────────────────────────────────────────────────
class PumpCard extends StatefulWidget {
  final String pumpId;
  final String pumpName;
  final String fbBase;       // Firebase base path for logs/alerts/schedule/cmd
  final String statusFbBase; // Firebase base path for status (relay1_state, online)
  final bool otherPumpOn;
  final String otherPumpName;
  final Future<void> Function(bool) onPumpToggle;
  final bool showSchedule;
  final String? masterStatusFbBase; // for pump2: path to pump1 status for mains detection

  const PumpCard({
    super.key,
    required this.pumpId,
    required this.pumpName,
    required this.fbBase,
    required this.statusFbBase,
    required this.otherPumpOn,
    required this.otherPumpName,
    required this.onPumpToggle,
    this.showSchedule = true,
    this.masterStatusFbBase,
  });

  @override
  State<PumpCard> createState() => _PumpCardState();
}

class _PumpCardState extends State<PumpCard> {
  final db = FirebaseDatabase.instance;

  Map<String, dynamic> _alerts = {};
  bool? _relay1Cmd;   // null = waiting for Firebase data
  bool  _isRunning        = false;
  bool  _isOnline         = false;
  bool  _mainsOn          = true;   // false when v1/v2/v3 all < 50 V (mains cut, running on battery)
  int   _todayRunS        = 0;    // sum of run_s for today's completed runs
  int   _currentRunStartMs = 0;   // ms epoch when current run started (0 = unknown)
  Timer? _runTicker;

  // Schedule state
  bool      _schedExpanded = false;
  bool      _schedEnabled  = false;
  TimeOfDay _schedOnTime   = const TimeOfDay(hour: 8,  minute: 0);
  TimeOfDay _schedOffTime  = const TimeOfDay(hour: 18, minute: 0);

  @override
  void initState() {
    super.initState();
    _listenStatus();
    _listenAlerts();
    _listenSchedule();
    _listenTodayRun();
    // Refresh display every 5 min so current run elapsed time stays current
    _runTicker = Timer.periodic(const Duration(minutes: 5), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _runTicker?.cancel();
    super.dispose();
  }

  void _listenTodayRun() {
    final now = DateTime.now();
    final todayStartMs =
        DateTime(now.year, now.month, now.day).millisecondsSinceEpoch;
    // Relay on/off events are in alerts (bridge routes pump/XX/log → alerts)
    db
        .ref('${widget.fbBase}/alerts')
        .orderByKey()
        .limitToLast(200)
        .onValue
        .listen((event) {
      final data = event.snapshot.value as Map?;
      if (data == null || !mounted) return;
      int sum = 0;
      int latestOnMs = 0;
      for (final v in data.values) {
        final entry = Map<String, dynamic>.from(v as Map);
        final ev   = entry['event'] as String? ?? '';
        // Only relay on/off events — skip protection alerts, mains_restore etc.
        if (ev != 'on' && ev != 'off') continue;
        // Normalize ts to ms: firmware sends Unix seconds, bridge fallback sends ms
        final rawTs = (entry['ts'] as num?)?.toInt() ?? 0;
        final ts = rawTs > 0 && rawTs < 10000000000 ? rawTs * 1000 : rawTs;
        final runS = (entry['run_s'] as num?)?.toInt() ?? 0;
        if (ts >= todayStartMs && ev == 'off') sum += runS;
        if (ts >= todayStartMs && ev == 'on' && ts > latestOnMs) latestOnMs = ts;
      }
      if (mounted) {
        setState(() {
          _todayRunS         = sum;
          _currentRunStartMs = latestOnMs;
        });
      }
    });
  }

  String _fmtRunTime(int s) {
    if (s <= 0)   return '0m';
    if (s < 60)   return '${s}s';
    if (s < 3600) return '${s ~/ 60}m';
    final h = s ~/ 3600;
    final m = (s % 3600) ~/ 60;
    return m > 0 ? '${h}h ${m}m' : '${h}h';
  }

  void _listenStatus() {
    // If a separate master path is provided (pump2), mains state comes from
    // that listener only — don't let pump2's own status (which has no v1/v2/v3)
    // overwrite _mainsOn back to true.
    final bool useSeparateMaster = widget.masterStatusFbBase != null &&
        widget.masterStatusFbBase != widget.statusFbBase;

    db.ref('${widget.statusFbBase}/status').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        final s = Map<String, dynamic>.from(data as Map);
        final double v1 = ((s['v1'] ?? 0.0) as num).toDouble();
        final double v2 = ((s['v2'] ?? 0.0) as num).toDouble();
        final double v3 = ((s['v3'] ?? 0.0) as num).toDouble();
        setState(() {
          if (!useSeparateMaster) {
            // pump01: read relay1 fields and mains from its own status
            _relay1Cmd = (s['relay1_state']   ?? 0) == 1;
            _isRunning = (s['relay1_running'] ?? 0) == 1;
            _isOnline  = s['online'] ?? false;
            _mainsOn   = s.containsKey('v1') ? (v1 >= 50.0 || v2 >= 50.0 || v3 >= 50.0) : true;
          }
          // pump02: relay state/running/mains come from master listener below
        });
      }
    });
    // For pump2: listen to pump1 status — reads relay2_state/relay2_running + mains
    if (useSeparateMaster) {
      db.ref('${widget.masterStatusFbBase}/status').onValue.listen((event) {
        final data = event.snapshot.value;
        if (data != null && mounted) {
          final s = Map<String, dynamic>.from(data as Map);
          final double v1 = ((s['v1'] ?? 0.0) as num).toDouble();
          final double v2 = ((s['v2'] ?? 0.0) as num).toDouble();
          final double v3 = ((s['v3'] ?? 0.0) as num).toDouble();
          setState(() {
            _relay1Cmd = (s['relay2_state']   ?? 0) == 1;
            _isRunning = (s['relay2_running'] ?? 0) == 1;
            _isOnline  = s['online'] ?? false;
            if (s.containsKey('v1')) {
              _mainsOn = v1 >= 50.0 || v2 >= 50.0 || v3 >= 50.0;
            }
          });
        }
      });
    }
  }

  void _listenAlerts() {
    db.ref('${widget.fbBase}/alerts').onValue.listen((event) {
      final data = event.snapshot.value as Map?;
      if (data == null || !mounted) return;
      // alerts is a push log — find the most recent entry that contains
      // protection fields (overvoltage etc.). Push keys are time-ordered
      // so the last matching entry is the most recent alert state.
      Map<String, dynamic>? latest;
      for (final v in data.values) {
        if (v is Map && v.containsKey('overvoltage')) {
          latest = Map<String, dynamic>.from(v);
        }
      }
      if (latest != null && mounted) {
        setState(() => _alerts = latest!);
      }
    });
  }

  void _listenSchedule() {
    db.ref('${widget.fbBase}/schedule').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        final s = Map<String, dynamic>.from(data as Map);
        setState(() {
          _schedEnabled = s['enabled'] ?? false;
          _schedOnTime  = TimeOfDay(
              hour: (s['on_hour']  ?? 8)  as int,
              minute: (s['on_min']  ?? 0) as int);
          _schedOffTime = TimeOfDay(
              hour: (s['off_hour'] ?? 18) as int,
              minute: (s['off_min'] ?? 0) as int);
        });
      }
    });
  }

  Future<void> _saveSchedule() async {
    // Conflict check: prevent same-site pumps having the same ON time
    if (_schedEnabled) {
      final site = kSites.firstWhere(
        (s) => s.pumpIds.contains(widget.pumpId),
        orElse: () => kSites.first,
      );
      for (final otherId in site.pumpIds.where((p) => p != widget.pumpId)) {
        final otherIdx    = site.pumpIds.indexOf(otherId);
        final otherFbBase = site.pumpFbPaths[otherIdx];
        final otherSnap   = await db.ref('$otherFbBase/schedule').get();
        if (otherSnap.exists) {
          final other = Map<String, dynamic>.from(otherSnap.value as Map);
          final otherEnabled = other['enabled'] ?? false;
          if (otherEnabled) {
            final otherOnHour = (other['on_hour'] ?? 8) as int;
            final otherOnMin  = (other['on_min']  ?? 0) as int;
            if (otherOnHour == _schedOnTime.hour &&
                otherOnMin  == _schedOnTime.minute) {
              if (mounted) {
                final otherIdx = site.pumpIds.indexOf(otherId);
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(
                    content: Text(
                      'Conflict: Pump ${otherIdx + 1} '
                      'already turns ON at ${_fmt(_schedOnTime)}. '
                      'Choose a different time.',
                    ),
                    backgroundColor: Colors.red.shade700,
                    duration: const Duration(seconds: 4),
                  ),
                );
              }
              return; // block save
            }
          }
        }
      }
    }

    await db.ref('${widget.fbBase}/schedule').set({
      'enabled':  _schedEnabled,
      'on_hour':  _schedOnTime.hour,
      'on_min':   _schedOnTime.minute,
      'off_hour': _schedOffTime.hour,
      'off_min':  _schedOffTime.minute,
    });
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text('Schedule saved'),
          duration: Duration(seconds: 2),
        ),
      );
    }
  }

  Future<void> _pickTime(bool isOnTime) async {
    final picked = await showTimePicker(
      context: context,
      initialTime: isOnTime ? _schedOnTime : _schedOffTime,
    );
    if (picked != null && mounted) {
      setState(() {
        if (isOnTime) { _schedOnTime  = picked; }
        else          { _schedOffTime = picked; }
      });
    }
  }

  String _fmt(TimeOfDay t) =>
      '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';

  @override
  Widget build(BuildContext context) {
    final bool alertOV  = _alerts['overvoltage']  ?? false;
    final bool alertUV  = _alerts['undervoltage'] ?? false;
    final bool alertPL  = _alerts['phase_loss']   ?? false;
    final bool alertDR  = _alerts['dry_run_trip'] ?? false;
    final bool anyAlert = alertOV || alertUV || alertPL || alertDR;

    // Resolve nullable relay state: null = waiting for Firebase data
    final bool loading  = _relay1Cmd == null;
    final bool relayOn  = _relay1Cmd == true;
    final Color stateColor = loading
        ? Colors.grey
        : !_isOnline
            ? Colors.grey
            : !_mainsOn
                ? Colors.orange
                : relayOn
                    ? (_isRunning ? Colors.green : Colors.orange)
                    : Colors.red;
    final String stateText = loading
        ? '---'
        : !_isOnline
            ? '---'
            : !_mainsOn
                ? 'STOPPED'
                : relayOn
                    ? (_isRunning ? 'RUNNING' : 'STARTING...')
                    : 'STOPPED';

    return Card(
      elevation: 2,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [

            // ── Header ───────────────────────────────────────────────────────
            Row(
              children: [
                Text(widget.pumpName,
                    style: const TextStyle(
                        fontSize: 16, fontWeight: FontWeight.bold)),
                const Spacer(),
                const Icon(Icons.timer_outlined, size: 14, color: Colors.blueGrey),
                const SizedBox(width: 4),
                Text(
                  'Today: ${_fmtRunTime(_todayRunS + (_isRunning && _currentRunStartMs > 0 ? (DateTime.now().millisecondsSinceEpoch - _currentRunStartMs) ~/ 1000 : 0))}${_isRunning ? ' +' : ''}',
                  style: const TextStyle(fontSize: 12, color: Colors.blueGrey),
                ),
              ],
            ),
            const Divider(),

            // ── Two-column: left = status indicator, right = relay control ──
            const SizedBox(height: 8),
            Row(
              crossAxisAlignment: CrossAxisAlignment.center,
              children: [
                // ── Left: status indicator ──────────────────────────────────
                Expanded(
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Container(
                        padding: const EdgeInsets.all(20),
                        // decoration: BoxDecoration(
                        //   shape: BoxShape.rectangle,
                        //   color: stateColor.withValues(alpha: 0.1),
                        //   border: Border.all(color: stateColor, width: 2.5),
                        // ),
                        child: Icon(
                          relayOn ? Icons.water_drop : Icons.water_drop_outlined,
                          size: 52,
                          color: stateColor,
                        ),
                      ),
                      const SizedBox(height: 6),
                      Text(
                        stateText,
                        style: TextStyle(
                          fontSize: 13,
                          fontWeight: FontWeight.bold,
                          color: stateColor,
                          letterSpacing: 1,
                        ),
                      ),
                    ],
                  ),
                ),

                // ── Divider between columns ─────────────────────────────────
                Container(
                  height: 100,
                  width: 1,
                  color: Colors.grey.shade200,
                  margin: const EdgeInsets.symmetric(horizontal: 12),
                ),

                // ── Right: relay control ────────────────────────────────────
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      // const Text('Relay Control',
                      //     textAlign: TextAlign.center,
                      //     style: TextStyle(
                      //         fontWeight: FontWeight.w600, fontSize: 13)),
                      const SizedBox(height: 8),
                      _RelayButton(
                        label: 'Pump',
                        isOn: relayOn,
                        disabled: loading || !_isOnline || !_mainsOn ||
                            (widget.otherPumpOn && !relayOn),
                        onToggle: (val) {
                          setState(() => _relay1Cmd = val);
                          widget.onPumpToggle(val);
                        },
                      ),
                      const SizedBox(height: 6),
                      if (!loading && !_isOnline)
                        const Row(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            Icon(Icons.cloud_off, size: 13, color: Colors.grey),
                            SizedBox(width: 4),
                            Flexible(
                              child: Text('Device offline',
                                  style: TextStyle(
                                      fontSize: 11, color: Colors.grey)),
                            ),
                          ],
                        ),
                      if (widget.otherPumpOn && !relayOn && !loading)
                        Row(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            const Icon(Icons.info_outline,
                                size: 13, color: Colors.orange),
                            const SizedBox(width: 4),
                            Flexible(
                              child: Text(
                                'Turn off ${widget.otherPumpName} first',
                                style: const TextStyle(
                                    fontSize: 11, color: Colors.orange),
                              ),
                            ),
                          ],
                        ),
                    ],
                  ),
                ),
              ],
            ),

            const SizedBox(height: 12),

            // ── Alerts ────────────────────────────────────────────────────────
            if (anyAlert) ...[
              Container(
                width: double.infinity,
                padding: const EdgeInsets.all(10),
                decoration: BoxDecoration(
                  color: Colors.red.shade50,
                  borderRadius: BorderRadius.circular(8),
                  border: Border.all(color: Colors.red.shade200),
                ),
                child: Wrap(
                  spacing: 8,
                  children: [
                    if (alertOV) const _AlertChip(label: 'Overvoltage'),
                    if (alertUV) const _AlertChip(label: 'Undervoltage'),
                    if (alertPL) const _AlertChip(label: 'Phase Loss'),
                    if (alertDR) const _AlertChip(label: 'Dry Run Trip'),
                  ],
                ),
              ),
              const SizedBox(height: 12),
            ],

            // ── Schedule section ──────────────────────────────────────────────
            if (widget.showSchedule) ...[
              InkWell(
                onTap: () => setState(() => _schedExpanded = !_schedExpanded),
                borderRadius: BorderRadius.circular(8),
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                  decoration: BoxDecoration(
                    color: Colors.blue.shade50,
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: Colors.blue.shade200),
                  ),
                  child: Row(
                    children: [
                      const Icon(Icons.schedule, size: 18, color: Colors.blue),
                      const SizedBox(width: 6),
                      const Text('Schedule',
                          style: TextStyle(fontWeight: FontWeight.w600, color: Colors.blue)),
                      const Spacer(),
                      if (_schedEnabled)
                        Text('${_fmt(_schedOnTime)} – ${_fmt(_schedOffTime)}',
                            style: const TextStyle(fontSize: 12, color: Colors.blue)),
                      const SizedBox(width: 6),
                      Icon(
                        _schedExpanded ? Icons.expand_less : Icons.expand_more,
                        color: Colors.blue,
                      ),
                    ],
                  ),
                ),
              ),
              if (_schedExpanded) ...[
                const SizedBox(height: 10),
                // Enable toggle
                Row(
                  children: [
                    const Text('Enable schedule'),
                    const Spacer(),
                    Switch(
                      value: _schedEnabled,
                      onChanged: (v) => setState(() => _schedEnabled = v),
                    ),
                  ],
                ),
                if (_schedEnabled) ...[
                  const SizedBox(height: 6),
                  // ON time row
                  Row(
                    children: [
                      const Icon(Icons.power_settings_new,
                          size: 16, color: Colors.green),
                      const SizedBox(width: 6),
                      const Text('Turn ON at'),
                      const Spacer(),
                      TextButton(
                        onPressed: () => _pickTime(true),
                        child: Text(_fmt(_schedOnTime),
                            style: const TextStyle(
                                fontSize: 16, fontWeight: FontWeight.bold)),
                      ),
                    ],
                  ),
                  // OFF time row
                  Row(
                    children: [
                      const Icon(Icons.power_off, size: 16, color: Colors.red),
                      const SizedBox(width: 6),
                      const Text('Turn OFF at'),
                      const Spacer(),
                      TextButton(
                        onPressed: () => _pickTime(false),
                        child: Text(_fmt(_schedOffTime),
                            style: const TextStyle(
                                fontSize: 16, fontWeight: FontWeight.bold)),
                      ),
                    ],
                  ),
                ],
                const SizedBox(height: 4),
                SizedBox(
                  width: double.infinity,
                  child: ElevatedButton.icon(
                    icon: const Icon(Icons.save, size: 16),
                    label: const Text('Save Schedule'),
                    onPressed: _saveSchedule,
                  ),
                ),
              ],
            ],
          ],
        ),
      ),
    );
  }
}

// ─── Shared power meter card ─────────────────────────────────────────────────
class PowerMeterCard extends StatefulWidget {
  final String fbPath;    // Firebase base path for master device
  final String simPhone;  // SIM phone number for SMS reset
  const PowerMeterCard({super.key, required this.fbPath, required this.simPhone});
  @override
  State<PowerMeterCard> createState() => _PowerMeterCardState();
}

class _PowerMeterCardState extends State<PowerMeterCard> {
  final db = FirebaseDatabase.instance;
  Map<String, dynamic> _status = {};
  List<Map<String, dynamic>> _mainsOutages = [];
  StreamSubscription? _logsSub;

  @override
  void initState() {
    super.initState();
    db.ref('${widget.fbPath}/status').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        setState(() => _status = Map<String, dynamic>.from(data as Map));
      }
    });
    _listenLogs();
  }

  @override
  void dispose() {
    _logsSub?.cancel();
    super.dispose();
  }

  void _listenLogs() {
    final now = DateTime.now();
    final cutoffS = DateTime(now.year, now.month, now.day).millisecondsSinceEpoch ~/ 1000;
    // mains_restore events go via pump/XX/log → bridge → Firebase alerts
    _logsSub = db
        .ref('${widget.fbPath}/alerts')
        .orderByKey()
        .limitToLast(200)
        .onValue
        .listen((event) {
      final map = event.snapshot.value as Map? ?? {};
      final outages = map.values
          .whereType<Map>()
          .where((e) => e['event'] == 'mains_restore')
          .where((e) {
            // Normalize: firmware sends seconds, bridge fallback sends ms
            final raw = ((e['ts'] ?? 0) as num).toInt();
            final tsS = raw >= 10000000000 ? raw ~/ 1000 : raw;
            return tsS >= cutoffS;
          })
          .toList()
        ..sort((a, b) {
            int tsS(Map e) {
              final raw = ((e['ts'] ?? 0) as num).toInt();
              return raw >= 10000000000 ? raw ~/ 1000 : raw;
            }
            return tsS(b).compareTo(tsS(a));
          });
      if (mounted) {
        setState(() => _mainsOutages = outages.cast<Map<String, dynamic>>());
      }
    });
  }

  String _fmtDuration(int seconds) {
    if (seconds >= 3600) {
      return '${seconds ~/ 3600}h ${(seconds % 3600) ~/ 60}m';
    }
    if (seconds >= 60) return '${seconds ~/ 60}m';
    return '<1m';
  }

  String _fmtTime(int epochMs) {
    final dt = DateTime.fromMillisecondsSinceEpoch(epochMs).toLocal();
    return '${dt.hour.toString().padLeft(2, '0')}:${dt.minute.toString().padLeft(2, '0')}';
  }

  Future<void> _resetDevice() async {
    final choice = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Reset Device?'),
        content: const Text(
            'This will reboot the controller board. Relays will turn OFF briefly during restart.'),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, null),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () => Navigator.pop(ctx, 'mqtt'),
            child: const Text('MQTT Reset'),
          ),
          TextButton(
            onPressed: () => Navigator.pop(ctx, 'sms'),
            child: const Text('SMS Reset', style: TextStyle(color: Colors.red)),
          ),
        ],
      ),
    );
    if (choice == null || !mounted) return;
    if (choice == 'sms') {
      final uri = Uri(
        scheme: 'sms',
        path: widget.simPhone,
        queryParameters: {'body': 'RESET'},
      );
      await launchUrl(uri);
    } else {
      await db.ref('${widget.fbPath}/cmd').set({
        'reset': 1,
        'src': 'app',
        'ts': DateTime.now().millisecondsSinceEpoch,
      });
    }
  }

  int _rssiToDbm(int rssi) => rssi == 99 ? 0 : -113 + rssi * 2;

  IconData _signalIcon(int rssi) {
    if (rssi == 99 || rssi < 6) return Icons.signal_cellular_0_bar;
    if (rssi < 15)              return Icons.signal_cellular_alt;
    return                             Icons.signal_cellular_4_bar;
  }

  Color _signalColor(int rssi) {
    if (rssi == 99 || rssi < 6) return Colors.grey;
    if (rssi < 10)              return Colors.red;
    if (rssi < 15)              return Colors.orange;
    return Colors.green;
  }

  IconData _batIcon(int pct) {
    if (pct > 80) return Icons.battery_full;
    if (pct > 60) return Icons.battery_5_bar;
    if (pct > 40) return Icons.battery_3_bar;
    if (pct > 20) return Icons.battery_2_bar;
    return Icons.battery_1_bar;
  }

  Color _batColor(int pct) {
    if (pct > 50) return Colors.green;
    if (pct > 20) return Colors.orange;
    return Colors.red;
  }

  @override
  Widget build(BuildContext context) {
    final double v1      = (_status['v1']      ?? 0.0).toDouble();
    final double v2      = (_status['v2']      ?? 0.0).toDouble();
    final double v3      = (_status['v3']      ?? 0.0).toDouble();
    final double current = (_status['current'] ?? 0.0).toDouble();
    final bool isOnline  = _status['online']   ?? false;
    final int rssi       = (_status['rssi']    ?? 99) as int;
    final int bat        = ((_status['bat']    ?? 255) as num).toInt();
    final bool hasBat    = bat <= 100;
    final bool noPower   = _status.containsKey('v1') && v1 < 50.0 && v2 < 50.0 && v3 < 50.0;
    final int outageAgeSec = ((_status['mains_dur_s'] ?? 0) as num).toInt();
    final Color onlineColor = isOnline
        ? (noPower ? Colors.orange : Colors.green)
        : Colors.red;

    return Card(
      elevation: 2,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            // Header: title + signal + online
            Row(
              children: [
                const Icon(Icons.electric_meter, color: Colors.orange),
                const SizedBox(width: 8),
                const Text('Power Meter',
                    style: TextStyle(
                        fontSize: 16, fontWeight: FontWeight.bold)),
                const Spacer(),
                Icon(_signalIcon(rssi),
                    color: _signalColor(rssi), size: 18),
                const SizedBox(width: 2),
                Text(
                  rssi == 99 ? '—' : '${_rssiToDbm(rssi)} dBm',
                  style: TextStyle(fontSize: 11, color: _signalColor(rssi)),
                ),
                if (hasBat) ...[
                  const SizedBox(width: 8),
                  Icon(_batIcon(bat), size: 16, color: _batColor(bat)),
                  const SizedBox(width: 2),
                  Text('$bat%',
                      style: TextStyle(fontSize: 11, color: _batColor(bat))),
                ],
                const SizedBox(width: 10),
                Icon(Icons.circle, color: onlineColor, size: 12),
                const SizedBox(width: 4),
                Text(
                  isOnline ? 'Online' : 'Offline',
                  style: TextStyle(color: onlineColor, fontSize: 13),
                ),
                const SizedBox(width: 8),
                IconButton(
                  icon: const Icon(Icons.restart_alt, size: 18),
                  color: isOnline ? Colors.blueGrey : Colors.grey.shade300,
                  padding: EdgeInsets.zero,
                  constraints: const BoxConstraints(),
                  tooltip: 'Reset device',
                  onPressed: isOnline ? _resetDevice : null,
                ),
              ],
            ),
            const Divider(),
            if (isOnline && noPower) ...[
              Container(
                margin: const EdgeInsets.only(bottom: 8),
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
                decoration: BoxDecoration(
                  color: Colors.orange.shade100,
                  borderRadius: BorderRadius.circular(6),
                  border: Border.all(color: Colors.orange),
                ),
                child: Row(children: [
                  const Icon(Icons.power_off, color: Colors.orange, size: 14),
                  const SizedBox(width: 6),
                  Text(
                    outageAgeSec > 0
                        ? 'Mains OFF — ${_fmtDuration(outageAgeSec)} on battery'
                        : 'Mains OFF — running on battery',
                    style: const TextStyle(
                        fontSize: 11,
                        color: Colors.orange,
                        fontWeight: FontWeight.w600),
                  ),
                ]),
              ),
            ],
            if (_mainsOutages.isNotEmpty) ...[
              Container(
                margin: const EdgeInsets.only(bottom: 8),
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                decoration: BoxDecoration(
                  color: Colors.grey.shade100,
                  borderRadius: BorderRadius.circular(6),
                  border: Border.all(color: Colors.grey.shade300),
                ),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text("Today's Outages",
                        style: TextStyle(
                            fontSize: 11,
                            fontWeight: FontWeight.bold,
                            color: Colors.grey.shade700)),
                    const SizedBox(height: 4),
                    ..._mainsOutages.map((e) {
                      final ts  = ((e['ts']         ?? 0) as num).toInt();
                      final dur = ((e['duration_s'] ?? 0) as num).toInt();
                      return Padding(
                        padding: const EdgeInsets.only(bottom: 2),
                        child: Text(
                          '• ${ts > 0 ? _fmtTime(ts * 1000) : '—'}  –  ${_fmtDuration(dur)}',
                          style: const TextStyle(fontSize: 11),
                        ),
                      );
                    }),
                    const Divider(height: 8, thickness: 0.5),
                    Text(
                      'Total: ${_fmtDuration(_mainsOutages.fold(0, (s, e) => s + ((e['duration_s'] ?? 0) as num).toInt()))}',
                      style: const TextStyle(
                          fontSize: 11, fontWeight: FontWeight.bold),
                    ),
                  ],
                ),
              ),
            ],
            const SizedBox(height: 8),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceAround,
              children: [
                _VoltageChip(label: 'L1', voltage: v1),
                _VoltageChip(label: 'L2', voltage: v2),
                _VoltageChip(label: 'L3', voltage: v3),
                _CurrentChip(current: current),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

// ─── Slave Status Card ────────────────────────────────────────────────────────
class SlaveStatusCard extends StatefulWidget {
  final String slaveFbPath; // Firebase base path for line2/pump1
  const SlaveStatusCard({super.key, required this.slaveFbPath});
  @override
  State<SlaveStatusCard> createState() => _SlaveStatusCardState();
}

class _SlaveStatusCardState extends State<SlaveStatusCard> {
  final db = FirebaseDatabase.instance;
  Map<String, dynamic> _s = {};
  StreamSubscription<DatabaseEvent>? _sub;
  bool _sending = false;

  @override
  void initState() {
    super.initState();
    _sub = db.ref('${widget.slaveFbPath}/status').onValue.listen((event) {
      final data = event.snapshot.value;
      if (mounted) setState(() => _s = data != null ? Map<String, dynamic>.from(data as Map) : {});
    });
  }

  Future<void> _toggleRelay(bool on) async {
    setState(() => _sending = true);
    // relay1 in app → bridge maps to relay3 (LoRa slave) via PUMP_CONFIGS cmdRelayMap
    await db.ref('${widget.slaveFbPath}/cmd').set({
      'relay1': on ? 1 : 0,
      'ts': DateTime.now().millisecondsSinceEpoch,
    });
    if (mounted) setState(() => _sending = false);
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_s.isEmpty) {
      return Card(
        elevation: 2,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
        child: const Padding(
          padding: EdgeInsets.all(16),
          child: Row(children: [
            Icon(Icons.electrical_services, color: Colors.blueGrey),
            SizedBox(width: 8),
            Text('Slave Unit', style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
            Spacer(),
            Text('No data', style: TextStyle(color: Colors.grey)),
          ]),
        ),
      );
    }

    final bool online  = _s['online']  == true;
    final int  relay   = ((_s['relay'] ?? 0)    as num).toInt();
    final int  rssi    = ((_s['rssi']  ?? 0)    as num).toInt();
    final double v1    = ((_s['v1']    ?? 0.0)  as num).toDouble();
    final double v2    = ((_s['v2']    ?? 0.0)  as num).toDouble();
    final double v3    = ((_s['v3']    ?? 0.0)  as num).toDouble();
    final double i1    = ((_s['i1']    ?? 0.0)  as num).toDouble();
    final double i2    = ((_s['i2']    ?? 0.0)  as num).toDouble();
    final double i3    = ((_s['i3']    ?? 0.0)  as num).toDouble();
    final double kw    = ((_s['kw']    ?? 0.0)  as num).toDouble();
    final int    kwh   = ((_s['kwh']   ?? 0)    as num).toInt();
    final double fl    = ((_s['fl']    ?? 0.0)  as num).toDouble();
    final int    tv    = ((_s['tv']    ?? 0)    as num).toInt();
    final int    dp    = ((_s['dp']    ?? -1)   as num).toInt();
    final bool hasEM   = v1 > 0 || v2 > 0 || v3 > 0;
    final bool noPower = _s.containsKey('v1') && v1 < 50.0 && v2 < 50.0 && v3 < 50.0;
    final int  bat     = ((_s['bat']  ?? 255) as num).toInt();
    final bool hasBat  = bat <= 100;
    final IconData batIcon = bat > 80 ? Icons.battery_full
                           : bat > 60 ? Icons.battery_5_bar
                           : bat > 40 ? Icons.battery_3_bar
                           : bat > 20 ? Icons.battery_2_bar
                           : Icons.battery_1_bar;
    final Color batColor = bat > 50 ? Colors.green
                         : bat > 20 ? Colors.orange
                         : Colors.red;

    final bool relayOn   = relay == 1;
    final bool isRunning = relayOn && fl > 0;
    final Color stateColor = !online
        ? Colors.grey
        : noPower
            ? Colors.orange
            : relayOn
                ? (isRunning ? Colors.green : Colors.orange)
                : Colors.red;
    final String stateText = !online
        ? '---'
        : noPower
            ? 'PWR OFF'
            : relayOn
                ? (isRunning ? 'RUNNING' : 'STARTING...')
                : 'STOPPED';
    final Color rssiColor = rssi < -95 ? Colors.red : rssi < -85 ? Colors.orange : Colors.green;

    final ShapeBorder cardBorder =
        RoundedRectangleBorder(borderRadius: BorderRadius.circular(12));

    return Column(
      children: [

        // ══ Power Meter Card ═══════════════════════════════════════════════
        Card(
          elevation: 2,
          shape: cardBorder,
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                // Header
                Row(
                  children: [
                    const Icon(Icons.electric_meter, color: Colors.orange),
                    const SizedBox(width: 8),
                    const Text('Power Meter',
                        style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
                    const Spacer(),
                    if (hasBat) ...[
                      Icon(batIcon, size: 16, color: batColor),
                      const SizedBox(width: 2),
                      Text('$bat%',
                          style: TextStyle(fontSize: 11, color: batColor)),
                      const SizedBox(width: 10),
                    ],
                    Icon(Icons.circle,
                        color: online ? Colors.green : Colors.red, size: 12),
                    const SizedBox(width: 4),
                    Text(online ? 'Online' : 'Offline',
                        style: TextStyle(
                            color: online ? Colors.green : Colors.red,
                            fontSize: 13)),
                  ],
                ),
                const Divider(),
                const SizedBox(height: 8),
                // Voltage + current chips
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceAround,
                  children: [
                    _VoltageChip(label: 'L1', voltage: v1),
                    _VoltageChip(label: 'L2', voltage: v2),
                    _VoltageChip(label: 'L3', voltage: v3),
                    _CurrentChip(current: (i1 + i2 + i3) / 3),
                  ],
                ),
                if (hasEM) ...[
                  const SizedBox(height: 10),
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceAround,
                    children: [
                      _SlaveChip(label: 'Power',  value: '${kw.toStringAsFixed(1)} kW',  icon: Icons.bolt,           color: Colors.amber),
                      _SlaveChip(label: 'Energy', value: '$kwh kWh',                      icon: Icons.electric_meter, color: Colors.deepOrange),
                    ],
                  ),
                ],
              ],
            ),
          ),
        ),

        const SizedBox(height: 8),

        // ══ Slave Unit Card ════════════════════════════════════════════════
        Card(
          elevation: 2,
          shape: cardBorder,
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [

                // ── Header ────────────────────────────────────────────────
                Row(
                  children: [
                    const Icon(Icons.electrical_services, color: Colors.blueGrey, size: 18),
                    const SizedBox(width: 6),
                    const Text('Slave Unit',
                        style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
                    const Spacer(),
                    if (online) ...[
                      Icon(Icons.router, size: 14, color: rssiColor),
                      const SizedBox(width: 3),
                      Text('$rssi dBm', style: TextStyle(fontSize: 11, color: rssiColor)),
                      const SizedBox(width: 8),
                      const Icon(Icons.water, size: 14, color: Colors.blueGrey),
                      const SizedBox(width: 3),
                      Text('$tv L',
                          style: const TextStyle(fontSize: 12, color: Colors.blueGrey)),
                      const SizedBox(width: 8),
                    ],
                    GestureDetector(
                      onTap: () => Navigator.push(
                        context,
                        MaterialPageRoute(builder: (_) =>
                            SlaveLogsPage(slaveFbPath: widget.slaveFbPath)),
                      ),
                      child: const Icon(Icons.bar_chart,
                          size: 20, color: Colors.blueGrey),
                    ),
                  ],
                ),
                const Divider(),

                // ── Two-column: state icon | relay control ─────────────────
                const SizedBox(height: 8),
                Row(
                  crossAxisAlignment: CrossAxisAlignment.center,
                  children: [
                    Expanded(
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          Container(
                            padding: const EdgeInsets.all(20),
                            child: Icon(
                              relayOn ? Icons.water_drop : Icons.water_drop_outlined,
                              size: 52,
                              color: stateColor,
                            ),
                          ),
                          const SizedBox(height: 6),
                          Text(stateText,
                              style: TextStyle(
                                fontSize: 13,
                                fontWeight: FontWeight.bold,
                                color: stateColor,
                                letterSpacing: 1,
                              )),
                        ],
                      ),
                    ),
                    Container(
                      height: 100,
                      width: 1,
                      color: Colors.grey.shade200,
                      margin: const EdgeInsets.symmetric(horizontal: 12),
                    ),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          const SizedBox(height: 8),
                          _RelayButton(
                            label: 'Slave',
                            isOn: relayOn,
                            disabled: !online || noPower || _sending,
                            onToggle: (val) => _toggleRelay(val),
                          ),
                          const SizedBox(height: 6),
                          if (!online)
                            const Row(
                              mainAxisAlignment: MainAxisAlignment.center,
                              children: [
                                Icon(Icons.cloud_off, size: 13, color: Colors.grey),
                                SizedBox(width: 4),
                                Flexible(
                                  child: Text('Device offline',
                                      style: TextStyle(fontSize: 11, color: Colors.grey)),
                                ),
                              ],
                            ),
                          if (online && noPower)
                            Container(
                              margin: const EdgeInsets.only(top: 6),
                              padding: const EdgeInsets.symmetric(
                                  horizontal: 8, vertical: 4),
                              decoration: BoxDecoration(
                                color: Colors.orange.shade100,
                                borderRadius: BorderRadius.circular(6),
                                border: Border.all(color: Colors.orange),
                              ),
                              child: const Row(children: [
                                Icon(Icons.warning_amber_rounded,
                                    color: Colors.orange, size: 14),
                                SizedBox(width: 4),
                                Flexible(
                                  child: Text('No power',
                                      style: TextStyle(
                                          fontSize: 11,
                                          color: Colors.orange,
                                          fontWeight: FontWeight.w600)),
                                ),
                              ]),
                            ),
                        ],
                      ),
                    ),
                  ],
                ),

                const SizedBox(height: 12),

                // ── Footer: flow + session + depth ─────────────────────────
                Container(
                  width: double.infinity,
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 10),
                  decoration: BoxDecoration(
                    color: Colors.blue.shade50,
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: Colors.blue.shade200),
                  ),
                  child: Row(
                    mainAxisAlignment: MainAxisAlignment.spaceAround,
                    children: [
                      _SlaveChip(
                        label: 'Flow Rate',
                        value: '${fl.toStringAsFixed(1)} L/min',
                        icon: Icons.speed,
                        color: fl > 0 ? Colors.blue : Colors.grey,
                      ),
                      _SlaveChip(
                        label: 'Session',
                        value: '$tv L',
                        icon: Icons.water,
                        color: Colors.teal,
                      ),
                      _SlaveChip(
                        label: 'Depth',
                        value: dp < 0 ? '--' : '${(dp / 1000.0).toStringAsFixed(2)} m',
                        icon: Icons.water_drop,
                        color: dp < 0 ? Colors.grey : Colors.indigo,
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ),
      ],
    );
  }
}

class _SlaveChip extends StatelessWidget {
  final String label;
  final String value;
  final IconData icon;
  final Color color;
  const _SlaveChip({required this.label, required this.value, required this.icon, required this.color});

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Icon(icon, color: color, size: 20),
        const SizedBox(height: 4),
        Text(value, style: TextStyle(fontSize: 14, fontWeight: FontWeight.bold, color: color)),
        Text(label, style: const TextStyle(fontSize: 11, color: Colors.grey)),
      ],
    );
  }
}

// ─── Voltage chip widget ──────────────────────────────────────────────────────
class _VoltageChip extends StatelessWidget {
  final String label;
  final double voltage;
  const _VoltageChip({required this.label, required this.voltage});

  Color _color() {
    if (voltage == 0) return Colors.grey;
    if (voltage > 460 || voltage < 360) return Colors.red;
    if (voltage < 390 || voltage > 440) return Colors.orange;
    return Colors.green;
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      decoration: BoxDecoration(
        color:  _color().withValues(alpha: 0.1),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: _color().withValues(alpha: 0.4)),
      ),
      child: Column(
        children: [
          Text(label,
              style: TextStyle(color: _color(), fontWeight: FontWeight.bold)),
          Text(voltage == 0 ? '--' : '${voltage.toStringAsFixed(1)}V',
              style: TextStyle(color: _color(), fontSize: 13)),
        ],
      ),
    );
  }
}

// ─── Current chip ─────────────────────────────────────────────────────────────
class _CurrentChip extends StatelessWidget {
  final double current;
  const _CurrentChip({required this.current});

  Color _color() {
    if (current <= 0.0) return Colors.grey;
    if (current > 20.0) return Colors.red;
    return Colors.orange;
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      decoration: BoxDecoration(
        color: _color().withValues(alpha: 0.1),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: _color().withValues(alpha: 0.4)),
      ),
      child: Column(
        children: [
          Text('I', style: TextStyle(color: _color(), fontWeight: FontWeight.bold)),
          Text('${current.toStringAsFixed(2)}A',
              style: TextStyle(color: _color(), fontSize: 13)),
        ],
      ),
    );
  }
}

// ─── Alert chip ───────────────────────────────────────────────────────────────
class _AlertChip extends StatelessWidget {
  final String label;
  const _AlertChip({required this.label});

  @override
  Widget build(BuildContext context) {
    return Chip(
      label: Text(label,
          style: const TextStyle(color: Colors.red, fontSize: 12)),
      backgroundColor: Colors.red.shade100,
      side: BorderSide(color: Colors.red.shade300),
      padding: EdgeInsets.zero,
    );
  }
}

// ─── Protection Settings page ─────────────────────────────────────────────────
class SettingsPage extends StatefulWidget {
  final List<String> pumpIds;
  final String?      slaveFbPath; // null when no slave on this site
  const SettingsPage({super.key, required this.pumpIds, this.slaveFbPath});
  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  final db = FirebaseDatabase.instance;
  final _formKey = GlobalKey<FormState>();

  final _ovCtrl     = TextEditingController();
  final _uvCtrl     = TextEditingController();
  final _plCtrl     = TextEditingController();
  final _dryICtrl   = TextEditingController();
  final _dryTCtrl   = TextEditingController();
  final _startTCtrl = TextEditingController();
  final _uvRstCtrl  = TextEditingController();

  late String _pumpId;  // selected pump
  int?   _selectedHp;               // null=custom, 5=5HP, 75=7.5HP
  bool   _dryRunEnabled = true;     // dry run optional

  static const _hpPresets = {
    5:  {'ov': 480.0, 'uv': 360.0, 'pl': 200.0, 'dry_i': 3.0, 'dry_t': 8},
    75: {'ov': 480.0, 'uv': 360.0, 'pl': 200.0, 'dry_i': 4.5, 'dry_t': 8},
  };

  bool _loading = true;
  bool _saving  = false;

  double? _devOv, _devUv, _devPl, _devDryI;
  int?    _devDryT, _devStartT, _devHp, _devDryEn, _devUvRst;

  // Relay2-specific "Active on device" values (pump02 only)
  double? _devDryI2;
  int?    _devDryT2, _devStartT2, _devHp2, _devDryEn2;

  // Global notification toggle (stored in SharedPreferences)
  bool _notifEnabled = true;
  // Schedule card visibility (stored in SharedPreferences)
  bool _showRotation = true;

  // Line 1 / Line 2 toggle (only shown when slaveFbPath != null)
  int _selectedLine = 1;

  // Slave (Line 2) settings state
  final _sOvCtrl     = TextEditingController();
  final _sUvCtrl     = TextEditingController();
  final _sPlCtrl     = TextEditingController();
  final _sDryICtrl   = TextEditingController();
  final _sDryTCtrl   = TextEditingController();
  final _sStartTCtrl = TextEditingController();
  final _sUvRstCtrl  = TextEditingController();
  final _slaveFormKey = GlobalKey<FormState>();
  int?  _sSelectedHp;
  bool  _sDryRunEnabled = true;
  bool  _sLoading       = true;
  bool  _sSaving        = false;
  StreamSubscription<DatabaseEvent>? _slaveSettingsSub;

  StreamSubscription<DatabaseEvent>? _settingsSub;
  StreamSubscription<DatabaseEvent>? _devSub;

  @override
  void initState() {
    super.initState();
    _pumpId = widget.pumpIds.isNotEmpty ? widget.pumpIds.first : 'pump01';
    _listenSettings();
    _listenDeviceSettings();
    _loadNotifPref();
    if (widget.slaveFbPath != null) _listenSlaveSettings();
  }

  Future<void> _loadNotifPref() async {
    final prefs = await SharedPreferences.getInstance();
    if (mounted) {
      setState(() {
        _notifEnabled = prefs.getBool('notifications_enabled') ?? true;
        _showRotation = prefs.getBool('show_rotation_card')    ?? false;
      });
    }
  }

  Future<void> _setShowRotation(bool enabled) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool('show_rotation_card', enabled);
    if (mounted) setState(() => _showRotation = enabled);
  }

  Future<void> _setNotifEnabled(bool enabled) async {
    final messaging = FirebaseMessaging.instance;
    final prefs = await SharedPreferences.getInstance();
    for (final site in kSites) {
      for (final fbBase in site.pumpFbPaths) {
        final topic = fcmTopicFromFbPath(fbBase);
        if (enabled) { await messaging.subscribeToTopic(topic); }
        else         { await messaging.unsubscribeFromTopic(topic); }
      }
      // Slave path cached by _PumpDashboardState._cacheSlavePath
      final slavePath = prefs.getString('slave_path_${site.id}');
      if (slavePath != null) {
        final topic = fcmTopicFromFbPath(slavePath);
        if (enabled) { await messaging.subscribeToTopic(topic); }
        else         { await messaging.unsubscribeFromTopic(topic); }
      }
    }
    await prefs.setBool('notifications_enabled', enabled);
    if (mounted) setState(() => _notifEnabled = enabled);
  }

  void _listenSlaveSettings() {
    _slaveSettingsSub?.cancel();
    _slaveSettingsSub = db.ref('${widget.slaveFbPath}/settings').onValue.listen((event) {
      if (_sSaving || !mounted) return;
      final s = event.snapshot.value as Map?;
      setState(() {
        _sSelectedHp      = (s?['hp']     as num?)?.toInt();
        _sDryRunEnabled   = (s?['dry_en'] as num?)?.toInt() != 0;
        _sOvCtrl.text     = (s?['ov']     ?? 480).toString();
        _sUvCtrl.text     = (s?['uv']     ?? 360).toString();
        _sPlCtrl.text     = (s?['pl']     ?? 200).toString();
        final rawDryI     = (s?['dry_i']  as num?)?.toDouble() ?? 0.0;
        _sDryICtrl.text   = (rawDryI > 0 ? rawDryI : 1.5).toString();
        final rawDryT     = (s?['dry_t']  as num?)?.toInt() ?? 0;
        _sDryTCtrl.text   = (rawDryT > 0 ? rawDryT : 8).toString();
        _sStartTCtrl.text = (s?['start_t'] ?? 90).toString();
        _sUvRstCtrl.text  = (s?['uv_rst']  ?? 300).toString();
        _sLoading = false;
      });
    });
  }

  Future<void> _saveSlave() async {
    if (!_slaveFormKey.currentState!.validate()) return;
    final ov     = double.parse(_sOvCtrl.text);
    final uv     = double.parse(_sUvCtrl.text);
    final pl     = double.parse(_sPlCtrl.text);
    final dryI   = double.parse(_sDryICtrl.text);
    final dryT   = int.parse(_sDryTCtrl.text);
    final startT = int.tryParse(_sStartTCtrl.text) ?? 90;
    final uvRst  = int.tryParse(_sUvRstCtrl.text)  ?? 300;
    setState(() => _sSaving = true);
    await db.ref('${widget.slaveFbPath}/settings').set({
      'ov': ov, 'uv': uv, 'pl': pl, 'uv_rst': uvRst,
      'dry_i': dryI, 'dry_t': dryT, 'start_t': startT,
      'dry_en': _sDryRunEnabled ? 1 : 0,
      if (_sSelectedHp != null) 'hp': _sSelectedHp,
    });
    setState(() => _sSaving = false);
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Line 2 settings saved')),
      );
    }
  }

  @override
  void dispose() {
    _settingsSub?.cancel();
    _devSub?.cancel();
    _slaveSettingsSub?.cancel();
    _ovCtrl.dispose(); _uvCtrl.dispose(); _plCtrl.dispose();
    _dryICtrl.dispose(); _dryTCtrl.dispose();
    _startTCtrl.dispose(); _uvRstCtrl.dispose();
    _sOvCtrl.dispose(); _sUvCtrl.dispose(); _sPlCtrl.dispose();
    _sDryICtrl.dispose(); _sDryTCtrl.dispose();
    _sStartTCtrl.dispose(); _sUvRstCtrl.dispose();
    super.dispose();
  }

  // true when the selected pump is relay2 on its site (e.g. pump02 on site01).
  bool get _isRelay2 {
    final site = kSites.firstWhere((s) => s.pumpIds.contains(_pumpId),
        orElse: () => kSites.first);
    return site.pumpIds.indexOf(_pumpId) == 1;
  }

  // Firebase base path for the selected pump
  String get _pumpFbBase {
    final site = kSites.firstWhere((s) => s.pumpIds.contains(_pumpId),
        orElse: () => kSites.first);
    final idx = site.pumpIds.indexOf(_pumpId);
    return idx >= 0 ? site.pumpFbPaths[idx] : site.masterFbBase;
  }

  // settings live at pump's own path; cfg status always from the master device
  String get _settingsPath => '$_pumpFbBase/settings';
  String get _statusPath {
    final site = kSites.firstWhere((s) => s.pumpIds.contains(_pumpId),
        orElse: () => kSites.first);
    return '${site.masterFbBase}/status';
  }

  void _listenSettings() {
    _settingsSub?.cancel();
    setState(() => _loading = true);
    _settingsSub = db.ref(_settingsPath).onValue.listen((event) {
      if (_saving || !mounted) return;
      final s = event.snapshot.value as Map?;
      setState(() {
        _selectedHp    = (s?['hp']     as num?)?.toInt();
        // null dry_en → default true; 0 → false
        _dryRunEnabled = (s?['dry_en'] as num?)?.toInt() != 0;
        _ovCtrl.text   = (s?['ov']    ?? 480).toString();
        _uvCtrl.text   = (s?['uv']    ?? 360).toString();
        _plCtrl.text   = (s?['pl']    ?? 200).toString();
        // Fall back to defaults if value is missing or was previously zeroed
        final rawDryI = (s?['dry_i'] as num?)?.toDouble() ?? 0.0;
        _dryICtrl.text   = (rawDryI > 0 ? rawDryI : 1.5).toString();
        final rawDryT = (s?['dry_t'] as num?)?.toInt() ?? 0;
        _dryTCtrl.text   = (rawDryT > 0 ? rawDryT : 8).toString();
        _startTCtrl.text = (s?['start_t'] ?? 300).toString();
        _uvRstCtrl.text  = (s?['uv_rst']  ?? 300).toString();
        _loading = false;
      });
    });
  }

  void _listenDeviceSettings() {
    _devSub?.cancel();
    _devSub = db.ref(_statusPath).onValue.listen((event) {
      final s = event.snapshot.value as Map?;
      if (s == null || !mounted) return;
      setState(() {
        if (_isRelay2) {
          // pump02: read relay2-specific cfg fields published in pump01 status
          _devDryI2   = (s['cfg_dry_i2']   as num?)?.toDouble();
          _devDryT2   = (s['cfg_dry_t2']   as num?)?.toInt();
          _devStartT2 = (s['cfg_start_t2'] as num?)?.toInt();
          _devHp2     = (s['cfg_hp2']      as num?)?.toInt();
          _devDryEn2  = (s['cfg_dry_en2']  as num?)?.toInt();
        } else {
          _devOv     = (s['cfg_ov']      as num?)?.toDouble();
          _devUv     = (s['cfg_uv']      as num?)?.toDouble();
          _devPl     = (s['cfg_pl']      as num?)?.toDouble();
          _devDryI   = (s['cfg_dry_i']   as num?)?.toDouble();
          _devDryT   = (s['cfg_dry_t']   as num?)?.toInt();
          _devStartT = (s['cfg_start_t'] as num?)?.toInt();
          _devHp     = (s['cfg_hp']      as num?)?.toInt();
          _devDryEn  = (s['cfg_dry_en']  as num?)?.toInt();
          _devUvRst  = (s['cfg_uv_rst_t'] as num?)?.toInt();
        }
      });
    });
  }

  void _onPumpChanged(String pump) {
    _settingsSub?.cancel();
    _devSub?.cancel();
    setState(() {
      _pumpId = pump;
      _loading = true;
      _devOv = _devUv = _devPl = _devDryI = null;
      _devDryT = _devStartT = _devHp = _devDryEn = _devUvRst = null;
      _devDryI2 = null;
      _devDryT2 = _devStartT2 = _devHp2 = _devDryEn2 = null;
    });
    _listenSettings();
    _listenDeviceSettings();
  }

  Future<void> _save() async {
    if (!_formKey.currentState!.validate()) return;
    final ov   = double.parse(_ovCtrl.text);
    final uv   = double.parse(_uvCtrl.text);
    final pl   = double.parse(_plCtrl.text);
    // Always save the real threshold values — never zero them out.
    // Zeroing caused validation failures (Must be > 0) on the next enable.
    // Firmware ignores dry_i/dry_t when cfg_dry_en=0, so safe to keep values.
    final dryI   = double.parse(_dryICtrl.text);
    final dryT   = int.parse(_dryTCtrl.text);
    final startT = int.tryParse(_startTCtrl.text) ?? 90;
    final uvRst  = int.tryParse(_uvRstCtrl.text)  ?? 0;

    setState(() => _saving = true);
    await db.ref(_settingsPath).set({
      // Voltage thresholds only apply to relay1 (shared power meter on site).
      // pump02 (relay2) skips ov/uv/pl — bridge routes to pump/02/settings
      // which firmware handles via apply_settings2().
      if (!_isRelay2) ...{'ov': ov, 'uv': uv, 'pl': pl, 'uv_rst': uvRst},
      'dry_i': dryI, 'dry_t': dryT, 'start_t': startT,
      'dry_en': _dryRunEnabled ? 1 : 0,
      if (_selectedHp != null) 'hp': _selectedHp,
    });
    setState(() => _saving = false);
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Settings saved for Pump ${_pumpId.replaceAll('pump', '')}'),
      ));
    }
  }

  String? _validatePositive(String? v) {
    if (v == null || v.isEmpty) return 'Required';
    if (double.tryParse(v) == null || double.parse(v) <= 0) return 'Must be > 0';
    return null;
  }

  String? _validateOv(String? v) {
    final err = _validatePositive(v);
    if (err != null) return err;
    final uv = double.tryParse(_uvCtrl.text) ?? 0;
    if (double.parse(v!) <= uv) return 'Must be > undervoltage';
    return null;
  }

  String? _validateUv(String? v) {
    final err = _validatePositive(v);
    if (err != null) return err;
    final pl = double.tryParse(_plCtrl.text) ?? 0;
    if (double.parse(v!) <= pl) return 'Must be > phase loss';
    return null;
  }

  String? _validateInt(String? v) {
    if (v == null || v.isEmpty) return 'Required';
    if (int.tryParse(v) == null || int.parse(v) <= 0) return 'Must be a whole number > 0';
    return null;
  }

  Widget _field(String label, TextEditingController ctrl, String unit,
      String? Function(String?) validator, {String? hint}) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: TextFormField(
        controller: ctrl,
        decoration: InputDecoration(
          labelText: label,
          suffixText: unit,
          hintText: hint,
          border: const OutlineInputBorder(),
          isDense: true,
        ),
        keyboardType: const TextInputType.numberWithOptions(decimal: true),
        validator: validator,
      ),
    );
  }

  Widget _deviceRow(String label, String value) {
    return Row(
      children: [
        Text('$label: ', style: const TextStyle(fontSize: 12, color: Colors.grey)),
        Text(value, style: const TextStyle(fontSize: 12, color: Colors.blueGrey)),
      ],
    );
  }


  @override
  Widget build(BuildContext context) {
    final showLine2 = widget.slaveFbPath != null && _selectedLine == 2;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Protection Settings'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
      ),
      body: Column(
        children: [
          // ── Line 1 / Line 2 toggle (only when slave exists) ────────
          if (widget.slaveFbPath != null)
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 12, 16, 4),
              child: SegmentedButton<int>(
                segments: const [
                  ButtonSegment(value: 1, label: Text('Line 1')),
                  ButtonSegment(value: 2, label: Text('Line 2')),
                ],
                selected: {_selectedLine},
                onSelectionChanged: (s) => setState(() => _selectedLine = s.first),
              ),
            ),
          // ── Content ────────────────────────────────────────────────
          if (showLine2)
            Expanded(
              child: _sLoading
                  ? const Center(child: CircularProgressIndicator())
                  : _buildLine2Form(),
            )
          else
            Expanded(
              child: _loading
                  ? const Center(child: CircularProgressIndicator())
                  : _buildLine1Form(),
            ),
        ],
      ),
    );
  }

  Widget _buildLine1Form() {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Form(
        key: _formKey,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            // ── Pump selection ────────────────────────────────────
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text('Pump Selection',
                        style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                    const SizedBox(height: 12),
                    SegmentedButton<String>(
                      segments: widget.pumpIds.map((id) {
                        final num = id.replaceAll('pump', '');
                        return ButtonSegment(value: id, label: Text('Pump $num'));
                      }).toList(),
                      selected: {_pumpId},
                      onSelectionChanged: (s) => _onPumpChanged(s.first),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            // ── Notifications ─────────────────────────────────────
            Card(
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
                child: Row(
                  children: [
                    const Icon(Icons.notifications_outlined, size: 20),
                    const SizedBox(width: 10),
                    const Expanded(
                      child: Text('Push Notifications',
                          style: TextStyle(fontWeight: FontWeight.bold, fontSize: 15)),
                    ),
                    Switch(value: _notifEnabled, onChanged: _setNotifEnabled),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            // ── Schedule visibility ───────────────────────────────
            Card(
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
                child: Row(
                  children: [
                    const Icon(Icons.schedule, size: 20, color: Colors.teal),
                    const SizedBox(width: 10),
                    const Expanded(
                      child: Text('Show Schedule',
                          style: TextStyle(fontWeight: FontWeight.bold, fontSize: 15)),
                    ),
                    Switch(
                      value: _showRotation,
                      activeThumbColor: Colors.teal,
                      onChanged: _setShowRotation,
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            // ── HP rating ─────────────────────────────────────────
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text('Pump Rating',
                        style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                    const SizedBox(height: 12),
                    DropdownButtonFormField<int?>(
                      key: ValueKey(_selectedHp),
                      initialValue: _selectedHp,
                      decoration: const InputDecoration(
                        labelText: 'Select Rating',
                        border: OutlineInputBorder(),
                        isDense: true,
                      ),
                      items: const [
                        DropdownMenuItem(value: null, child: Text('Custom')),
                        DropdownMenuItem(value: 5,    child: Text('5 HP  (3.7 kW)')),
                        DropdownMenuItem(value: 75,   child: Text('7.5 HP  (5.6 kW)')),
                      ],
                      onChanged: (val) {
                        setState(() {
                          _selectedHp = val;
                          if (val != null && _hpPresets.containsKey(val)) {
                            final p = _hpPresets[val]!;
                            _ovCtrl.text   = p['ov'].toString();
                            _uvCtrl.text   = p['uv'].toString();
                            _plCtrl.text   = p['pl'].toString();
                            _dryICtrl.text = p['dry_i'].toString();
                            _dryTCtrl.text = p['dry_t'].toString();
                          }
                        });
                      },
                    ),
                    const SizedBox(height: 6),
                    const Text(
                      'Selecting a rating fills preset thresholds. You can still edit values below.',
                      style: TextStyle(fontSize: 11, color: Colors.grey),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            // ── Voltage protection ────────────────────────────────
            if (!_isRelay2)
              Card(
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text('Voltage Protection',
                          style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                      const SizedBox(height: 12),
                      _field('Over Voltage',  _ovCtrl, 'V', _validateOv,    hint: 'e.g. 480'),
                      _field('Under Voltage', _uvCtrl, 'V', _validateUv,    hint: 'e.g. 360'),
                      _field('Phase Loss',    _plCtrl, 'V', _validatePositive, hint: 'e.g. 200'),
                      const SizedBox(height: 6),
                      _field('UV/PL Auto-restart delay', _uvRstCtrl, 's',
                          (v) {
                            if (v == null || v.isEmpty) return 'Required';
                            final n = int.tryParse(v);
                            if (n == null || n < 0) return 'Must be 0 or more';
                            return null;
                          },
                          hint: '0 = disabled, e.g. 300'),
                      const Padding(
                        padding: EdgeInsets.only(bottom: 4),
                        child: Text(
                          '0 = disabled. When > 0, pump auto-restarts this many seconds after '
                          'undervoltage / phase loss clears. 300 s (5 min) recommended. '
                          'Overvoltage never auto-restarts.',
                          style: TextStyle(fontSize: 11, color: Colors.grey),
                        ),
                      ),
                    ],
                  ),
                ),
              )
            else
              Card(
                color: Colors.orange.shade50,
                child: const Padding(
                  padding: EdgeInsets.all(14),
                  child: Row(
                    children: [
                      Icon(Icons.info_outline, size: 18, color: Colors.orange),
                      SizedBox(width: 8),
                      Expanded(
                        child: Text(
                          'Voltage protection (OV / UV / Phase Loss) is shared — '
                          'configure it via Pump 1 settings.',
                          style: TextStyle(fontSize: 12, color: Colors.orange),
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            const SizedBox(height: 12),
            // ── Dry run protection ────────────────────────────────
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        const Text('Dry Run Protection',
                            style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                        const Spacer(),
                        Switch(
                          value: _dryRunEnabled,
                          onChanged: (v) => setState(() => _dryRunEnabled = v),
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    _field('Startup Delay', _startTCtrl, 's', _validateInt, hint: 'e.g. 90'),
                    const Padding(
                      padding: EdgeInsets.only(bottom: 6),
                      child: Text(
                        'Dry run detection is skipped for this many seconds after relay turns ON '
                        '— covers soft-starter / star-delta delay.',
                        style: TextStyle(fontSize: 11, color: Colors.grey),
                      ),
                    ),
                    if (_dryRunEnabled) ...[
                      _field('Current Threshold', _dryICtrl, 'A', _validatePositive,
                          hint: 'e.g. 3.0'),
                      _field('Trip Delay', _dryTCtrl, 's', _validateInt, hint: 'e.g. 8'),
                    ] else
                      const Padding(
                        padding: EdgeInsets.only(top: 8),
                        child: Text(
                          'Disabled — pump runs without current monitoring.',
                          style: TextStyle(fontSize: 12, color: Colors.grey),
                        ),
                      ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            // ── Active on device ──────────────────────────────────
            Card(
              color: Colors.blue.shade50,
              child: Padding(
                padding: const EdgeInsets.all(12),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text('Active on device',
                        style: TextStyle(fontWeight: FontWeight.bold,
                            fontSize: 13, color: Colors.blueGrey)),
                    const SizedBox(height: 6),
                    if (_isRelay2 ? (_devDryEn2 == null) : (_devOv == null))
                      const Text('Waiting for device status…',
                          style: TextStyle(fontSize: 12, color: Colors.grey))
                    else if (_isRelay2) ...[
                      if (_devHp2 != null && _devHp2! > 0)
                        _deviceRow('Rating', _devHp2 == 75 ? '7.5 HP' : '$_devHp2 HP'),
                      if (_devStartT2 != null)
                        _deviceRow('Startup delay', '$_devStartT2 s'),
                      _deviceRow('Dry Run', _devDryEn2 == 1 ? 'Enabled' : 'Disabled'),
                      if (_devDryEn2 == 1) ...[
                        _deviceRow('Dry I', '${_devDryI2?.toStringAsFixed(1)} A'),
                        _deviceRow('Dry T', '$_devDryT2 s'),
                      ],
                    ] else ...[
                      if (_devHp != null && _devHp! > 0)
                        _deviceRow('Rating', _devHp == 75 ? '7.5 HP' : '$_devHp HP'),
                      _deviceRow('OV', '${_devOv?.toStringAsFixed(0)} V'),
                      _deviceRow('UV', '${_devUv?.toStringAsFixed(0)} V'),
                      _deviceRow('PL', '${_devPl?.toStringAsFixed(0)} V'),
                      if (_devStartT != null)
                        _deviceRow('Startup delay', '$_devStartT s'),
                      if (_devDryEn != null)
                        _deviceRow('Dry Run', _devDryEn == 1 ? 'Enabled' : 'Disabled'),
                      if (_devDryEn == 1) ...[
                        _deviceRow('Dry I', '${_devDryI?.toStringAsFixed(1)} A'),
                        _deviceRow('Dry T', '$_devDryT s'),
                      ],
                      if (_devUvRst != null)
                        _deviceRow('UV Auto-restart',
                            _devUvRst == 0 ? 'Disabled' : '$_devUvRst s'),
                    ],
                  ],
                ),
              ),
            ),
            const SizedBox(height: 16),
            ElevatedButton(
              onPressed: _saving ? null : _save,
              style: ElevatedButton.styleFrom(
                padding: const EdgeInsets.symmetric(vertical: 14),
              ),
              child: _saving
                  ? const SizedBox(height: 20, width: 20,
                      child: CircularProgressIndicator(strokeWidth: 2))
                  : const Text('Save Settings',
                      style: TextStyle(fontWeight: FontWeight.bold)),
            ),
            const SizedBox(height: 8),
            const Text(
              'Settings are saved to Firebase and forwarded to the device via MQTT.\n'
              'The device applies them immediately and echoes back the active values above.',
              style: TextStyle(fontSize: 11, color: Colors.grey),
              textAlign: TextAlign.center,
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildLine2Form() {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Form(
        key: _slaveFormKey,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            // ── HP rating ─────────────────────────────────────────
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text('Pump Rating',
                        style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                    const SizedBox(height: 12),
                    DropdownButtonFormField<int?>(
                      key: ValueKey(_sSelectedHp),
                      initialValue: _sSelectedHp,
                      decoration: const InputDecoration(
                        labelText: 'Select Rating',
                        border: OutlineInputBorder(),
                        isDense: true,
                      ),
                      items: const [
                        DropdownMenuItem(value: null, child: Text('Custom')),
                        DropdownMenuItem(value: 5,    child: Text('5 HP  (3.7 kW)')),
                        DropdownMenuItem(value: 75,   child: Text('7.5 HP  (5.6 kW)')),
                      ],
                      onChanged: (val) {
                        setState(() {
                          _sSelectedHp = val;
                          if (val != null && _hpPresets.containsKey(val)) {
                            final p = _hpPresets[val]!;
                            _sOvCtrl.text   = p['ov'].toString();
                            _sUvCtrl.text   = p['uv'].toString();
                            _sPlCtrl.text   = p['pl'].toString();
                            _sDryICtrl.text = p['dry_i'].toString();
                            _sDryTCtrl.text = p['dry_t'].toString();
                          }
                        });
                      },
                    ),
                    const SizedBox(height: 6),
                    const Text(
                      'Selecting a rating fills preset thresholds. You can still edit values below.',
                      style: TextStyle(fontSize: 11, color: Colors.grey),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            // ── Voltage protection ────────────────────────────────
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text('Voltage Protection',
                        style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                    const SizedBox(height: 12),
                    _field('Over Voltage',  _sOvCtrl, 'V', (v) {
                      if (v == null || v.isEmpty) return 'Required';
                      if (double.tryParse(v) == null || double.parse(v) <= 0) return 'Must be > 0';
                      final uv = double.tryParse(_sUvCtrl.text) ?? 0;
                      if (double.parse(v) <= uv) return 'Must be > undervoltage';
                      return null;
                    }, hint: 'e.g. 480'),
                    _field('Under Voltage', _sUvCtrl, 'V', (v) {
                      if (v == null || v.isEmpty) return 'Required';
                      if (double.tryParse(v) == null || double.parse(v) <= 0) return 'Must be > 0';
                      final pl = double.tryParse(_sPlCtrl.text) ?? 0;
                      if (double.parse(v) <= pl) return 'Must be > phase loss';
                      return null;
                    }, hint: 'e.g. 360'),
                    _field('Phase Loss',    _sPlCtrl, 'V', _validatePositive, hint: 'e.g. 200'),
                    const SizedBox(height: 6),
                    _field('UV/PL Auto-restart delay', _sUvRstCtrl, 's', (v) {
                      if (v == null || v.isEmpty) return 'Required';
                      final n = int.tryParse(v);
                      if (n == null || n < 0) return 'Must be 0 or more';
                      return null;
                    }, hint: '0 = disabled, e.g. 300'),
                    const Text(
                      '0 = disabled. When > 0, pump auto-restarts this many seconds after '
                      'undervoltage / phase loss clears.',
                      style: TextStyle(fontSize: 11, color: Colors.grey),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            // ── Dry run protection ────────────────────────────────
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        const Text('Dry Run Protection',
                            style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                        const Spacer(),
                        Switch(
                          value: _sDryRunEnabled,
                          onChanged: (v) => setState(() => _sDryRunEnabled = v),
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    _field('Startup Delay', _sStartTCtrl, 's', _validateInt, hint: 'e.g. 90'),
                    const Padding(
                      padding: EdgeInsets.only(bottom: 6),
                      child: Text(
                        'Dry run detection is skipped for this many seconds after relay turns ON.',
                        style: TextStyle(fontSize: 11, color: Colors.grey),
                      ),
                    ),
                    if (_sDryRunEnabled) ...[
                      _field('Current Threshold', _sDryICtrl, 'A', _validatePositive,
                          hint: 'e.g. 3.0'),
                      _field('Trip Delay', _sDryTCtrl, 's', _validateInt, hint: 'e.g. 8'),
                    ] else
                      const Padding(
                        padding: EdgeInsets.only(top: 8),
                        child: Text(
                          'Disabled — pump runs without current monitoring.',
                          style: TextStyle(fontSize: 12, color: Colors.grey),
                        ),
                      ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 16),
            ElevatedButton(
              onPressed: _sSaving ? null : _saveSlave,
              style: ElevatedButton.styleFrom(
                padding: const EdgeInsets.symmetric(vertical: 14),
              ),
              child: _sSaving
                  ? const SizedBox(height: 20, width: 20,
                      child: CircularProgressIndicator(strokeWidth: 2))
                  : const Text('Save Line 2 Settings',
                      style: TextStyle(fontWeight: FontWeight.bold)),
            ),
            const SizedBox(height: 8),
            const Text(
              'Settings are saved to Firebase and forwarded to the slave device via MQTT.',
              style: TextStyle(fontSize: 11, color: Colors.grey),
              textAlign: TextAlign.center,
            ),
          ],
        ),
      ),
    );
  }
}


// ─── Relay toggle button ──────────────────────────────────────────────────────
class _RelayButton extends StatelessWidget {
  final String label;
  final bool isOn;
  final bool disabled;
  final ValueChanged<bool> onToggle;
  const _RelayButton(
      {required this.label, required this.isOn, this.disabled = false, required this.onToggle});

  @override
  Widget build(BuildContext context) {
    return ElevatedButton(
      style: ElevatedButton.styleFrom(
        backgroundColor: disabled
            ? Colors.grey.shade200
            : isOn ? Colors.green : Colors.grey.shade300,
        foregroundColor: disabled ? Colors.grey : isOn ? Colors.white : Colors.black87,
        padding: const EdgeInsets.symmetric(vertical: 14),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
      ),
      onPressed: disabled ? null : () => onToggle(!isOn),
      child: Text('$label\n${isOn ? "ON" : "OFF"}',
          textAlign: TextAlign.center,
          style: const TextStyle(fontWeight: FontWeight.bold)),
    );
  }
}
