// lib/main.dart
// Flutter pump controller app
// Reads pump/status from Firebase, writes pump/cmd

import 'package:flutter/material.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_database/firebase_database.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp(
    options: const FirebaseOptions(
      apiKey:            "AIzaSyBIwsgRvj82XbuxSN02n4pKIwKTz_zkpik",
      appId:             "1:22800348697:web:d62a0d90dea41a4f97dc88",
      messagingSenderId: "22800348697",
      projectId:         "pump-controller-4398d",
      databaseURL:       "https://pump-controller-4398d-default-rtdb.firebaseio.com",
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
      home: const PumpDashboard(),
    );
  }
}

// ─── Dashboard — mutual exclusion coordinator ─────────────────────────────────
class PumpDashboard extends StatefulWidget {
  const PumpDashboard({super.key});
  @override
  State<PumpDashboard> createState() => _PumpDashboardState();
}

class _PumpDashboardState extends State<PumpDashboard> {
  final db = FirebaseDatabase.instance;

  // Actual device relay states tracked here for mutual exclusion
  final Map<String, bool> _pumpOn = {'pump01': false, 'pump02': false};

  @override
  void initState() {
    super.initState();
    // Both pumps are on the same device — relay1 = pump01, relay2 = pump02
    db.ref('pumps/pump01/status/relay1_state').onValue.listen((event) {
      if (mounted) setState(() => _pumpOn['pump01'] = (event.snapshot.value ?? 0) == 1);
    });
    db.ref('pumps/pump01/status/relay2_state').onValue.listen((event) {
      if (mounted) setState(() => _pumpOn['pump02'] = (event.snapshot.value ?? 0) == 1);
    });
  }

  // Called by PumpCard when user presses the pump button.
  // Both pumps are on the same device: relay1 = pump01, relay2 = pump02.
  // Always writes to pumps/pump01/cmd so bridge forwards to pump/01/cmd.
  Future<void> _handlePumpToggle(String pumpId, bool turnOn) async {
    // Only send the field for the pump being toggled.
    // Sending both fields would pulse the wrong coil on the latching relay.
    final Map<String, dynamic> cmd = {'ts': DateTime.now().millisecondsSinceEpoch};
    if (pumpId == 'pump01') cmd['relay1'] = turnOn ? 1 : 0;
    if (pumpId == 'pump02') cmd['relay2'] = turnOn ? 1 : 0;
    await db.ref('pumps/pump01/cmd').set(cmd);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Pump Controller'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            const PowerMeterCard(),
            const SizedBox(height: 16),
            PumpCard(
              pumpId: 'pump01',
              pumpName: 'Pump 1',
              otherPumpOn: _pumpOn['pump02'] ?? false,
              otherPumpName: 'Pump 2',
              onPumpToggle: (val) => _handlePumpToggle('pump01', val),
            ),
            const SizedBox(height: 16),
            PumpCard(
              pumpId: 'pump02',
              pumpName: 'Pump 2',
              otherPumpOn: _pumpOn['pump01'] ?? false,
              otherPumpName: 'Pump 1',
              onPumpToggle: (val) => _handlePumpToggle('pump02', val),
            ),
            const SizedBox(height: 16),
            const RotationScheduleCard(),
          ],
        ),
      ),
    );
  }
}

// ─── Rotation schedule card ───────────────────────────────────────────────────
class RotationScheduleCard extends StatefulWidget {
  const RotationScheduleCard({super.key});
  @override
  State<RotationScheduleCard> createState() => _RotationScheduleCardState();
}

class _RotationScheduleCardState extends State<RotationScheduleCard> {
  final db = FirebaseDatabase.instance;

  bool   _enabled         = false;
  int    _intervalMinutes = 240; // default 4 h
  String _currentPump     = 'pump01';
  int    _startedAt       = 0;
  bool   _expanded        = false;

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
    db.ref('rotation_schedule').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        final s = Map<String, dynamic>.from(data as Map);
        setState(() {
          _enabled         = s['enabled']          ?? false;
          _intervalMinutes = (s['interval_minutes'] ?? 240) as int;
          _currentPump     = s['current_pump']      ?? 'pump01';
          _startedAt       = (s['started_at']       ?? 0) as int;
        });
      }
    });
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
    await db.ref('rotation_schedule').update({
      'enabled':          _enabled,
      'interval_minutes': _intervalMinutes,
    });
    if (!_enabled) {
      // clear started_at so it restarts cleanly when re-enabled
      await db.ref('rotation_schedule/started_at').set(0);
    }
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Rotation schedule saved'),
            duration: Duration(seconds: 2)));
    }
  }

  @override
  Widget build(BuildContext context) {
    final activePumpLabel = _currentPump == 'pump01' ? 'Pump 1' : 'Pump 2';
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
  final bool otherPumpOn;
  final String otherPumpName;
  final Future<void> Function(bool) onPumpToggle;

  const PumpCard({
    super.key,
    required this.pumpId,
    required this.pumpName,
    required this.otherPumpOn,
    required this.otherPumpName,
    required this.onPumpToggle,
  });

  @override
  State<PumpCard> createState() => _PumpCardState();
}

class _PumpCardState extends State<PumpCard> {
  final db = FirebaseDatabase.instance;

  Map<String, dynamic> _alerts = {};
  bool _relay1Cmd = false;

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
  }

  void _listenStatus() {
    // Both pumps are on the same device — all status comes from pumps/pump01/status.
    // pump01 uses relay1_state; pump02 uses relay2_state.
    db.ref('pumps/pump01/status').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        final s = Map<String, dynamic>.from(data as Map);
        setState(() {
          _relay1Cmd = widget.pumpId == 'pump02'
              ? (s['relay2_state'] ?? 0) == 1
              : (s['relay1_state'] ?? 0) == 1;
        });
      }
    });
  }

  void _listenAlerts() {
    db.ref('pumps/${widget.pumpId}/alerts').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        setState(() => _alerts = Map<String, dynamic>.from(data as Map));
      }
    });
  }

  void _listenSchedule() {
    db.ref('pumps/${widget.pumpId}/schedule').onValue.listen((event) {
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
    // Conflict check: prevent both pumps having the same ON time
    if (_schedEnabled) {
      final otherId = widget.pumpId == 'pump01' ? 'pump02' : 'pump01';
      final otherSnap = await db.ref('pumps/$otherId/schedule').get();
      if (otherSnap.exists) {
        final other = Map<String, dynamic>.from(otherSnap.value as Map);
        final otherEnabled = other['enabled'] ?? false;
        if (otherEnabled) {
          final otherOnHour = (other['on_hour'] ?? 8) as int;
          final otherOnMin  = (other['on_min']  ?? 0) as int;
          if (otherOnHour == _schedOnTime.hour &&
              otherOnMin  == _schedOnTime.minute) {
            if (mounted) {
              ScaffoldMessenger.of(context).showSnackBar(
                SnackBar(
                  content: Text(
                    'Conflict: ${otherId == "pump01" ? "Pump 1" : "Pump 2"} '
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

    await db.ref('pumps/${widget.pumpId}/schedule').set({
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

    return Card(
      elevation: 2,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [

            // ── Header ───────────────────────────────────────────────────────
            Text(widget.pumpName,
                style: const TextStyle(
                    fontSize: 20, fontWeight: FontWeight.bold)),
            const Divider(),

            // ── Pump status icon ──────────────────────────────────────────────
            const SizedBox(height: 8),
            Center(
              child: Column(
                children: [
                  Container(
                    padding: const EdgeInsets.all(20),
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: (_relay1Cmd ? Colors.green : Colors.red)
                          .withValues(alpha: 0.1),
                      border: Border.all(
                        color: _relay1Cmd ? Colors.green : Colors.red,
                        width: 2.5,
                      ),
                    ),
                    child: Icon(
                      _relay1Cmd
                          ? Icons.water_drop
                          : Icons.water_drop_outlined,
                      size: 52,
                      color: _relay1Cmd ? Colors.green : Colors.red,
                    ),
                  ),
                  const SizedBox(height: 6),
                  Text(
                    _relay1Cmd ? 'RUNNING' : 'STOPPED',
                    style: TextStyle(
                      fontSize: 13,
                      fontWeight: FontWeight.bold,
                      color: _relay1Cmd ? Colors.green : Colors.red,
                      letterSpacing: 1,
                    ),
                  ),
                ],
              ),
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

            // ── Pump button ───────────────────────────────────────────────────
            const Text('Relay Control',
                style: TextStyle(fontWeight: FontWeight.w600)),
            const SizedBox(height: 8),
            SizedBox(
              width: double.infinity,
              child: _RelayButton(
                label: 'Pump',
                isOn: _relay1Cmd,
                disabled: widget.otherPumpOn && !_relay1Cmd,
                onToggle: (val) {
                  setState(() => _relay1Cmd = val);
                  widget.onPumpToggle(val);
                },
              ),
            ),
            if (widget.otherPumpOn && !_relay1Cmd) ...[
              const SizedBox(height: 6),
              Row(
                children: [
                  const Icon(Icons.info_outline, size: 14, color: Colors.orange),
                  const SizedBox(width: 4),
                  Text(
                    'Turn off ${widget.otherPumpName} first',
                    style: const TextStyle(fontSize: 12, color: Colors.orange),
                  ),
                ],
              ),
            ],
            const SizedBox(height: 12),

            // ── Schedule section ──────────────────────────────────────────────
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
        ),
      ),
    );
  }
}

// ─── Shared power meter card ─────────────────────────────────────────────────
class PowerMeterCard extends StatefulWidget {
  const PowerMeterCard({super.key});
  @override
  State<PowerMeterCard> createState() => _PowerMeterCardState();
}

class _PowerMeterCardState extends State<PowerMeterCard> {
  final db = FirebaseDatabase.instance;
  Map<String, dynamic> _status = {};

  @override
  void initState() {
    super.initState();
    // Power meter is wired to pump01's board
    db.ref('pumps/pump01/status').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        setState(() => _status = Map<String, dynamic>.from(data as Map));
      }
    });
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

  @override
  Widget build(BuildContext context) {
    final double v1      = (_status['v1']      ?? 0.0).toDouble();
    final double v2      = (_status['v2']      ?? 0.0).toDouble();
    final double v3      = (_status['v3']      ?? 0.0).toDouble();
    final double current = (_status['current'] ?? 0.0).toDouble();
    final bool isOnline  = _status['online']   ?? false;
    final int rssi       = (_status['rssi']    ?? 99) as int;
    final Color onlineColor = isOnline ? Colors.green : Colors.red;

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
                const SizedBox(width: 10),
                Icon(Icons.circle, color: onlineColor, size: 12),
                const SizedBox(width: 4),
                Text(isOnline ? 'Online' : 'Offline',
                    style: TextStyle(color: onlineColor, fontSize: 13)),
              ],
            ),
            const Divider(),
            const Text('3-Phase Voltage',
                style: TextStyle(fontWeight: FontWeight.w600)),
            const SizedBox(height: 8),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceAround,
              children: [
                _VoltageChip(label: 'L1', voltage: v1),
                _VoltageChip(label: 'L2', voltage: v2),
                _VoltageChip(label: 'L3', voltage: v3),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                const Icon(Icons.bolt, size: 18, color: Colors.orange),
                const SizedBox(width: 4),
                Text('Current: ${current.toStringAsFixed(2)} A',
                    style: const TextStyle(fontSize: 15)),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

// ─── Voltage chip widget ──────────────────────────────────────────────────────
class _VoltageChip extends StatelessWidget {
  final String label;
  final double voltage;
  const _VoltageChip({required this.label, required this.voltage});

  Color _color() {
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
          Text('${voltage.toStringAsFixed(1)}V',
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
