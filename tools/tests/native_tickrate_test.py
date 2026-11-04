"""Replay compiled VM timing, simulation dispatch and snapshot history without starting a game.

Build tickrate_layout.c with the same headers and pass its JSON output alongside
an unstripped Windows or Linux dedicated server. Requires pefile and unicorn.
Synthetic dependencies supply allocation, userinfo, string-release, error, CRT,
terrain and wire-output callbacks. VM conversion, lifecycle, vehicle integration,
missile dispatch, ping averaging and snapshot fallback execute compiled server code.
"""
import argparse
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_FPCW, UC_X86_REG_MXCSR


class Native:
    scratch, stack, stop = 0x20000000, 0x30008000, 0x200ff000

    def __init__(self, path, layout):
        self.cpu = Uc(UC_ARCH_X86, UC_MODE_32)
        self.layout = layout
        data = path.read_bytes()
        self.symbols = {}
        if data[:2] == b'MZ':
            pe = pefile.PE(data=data)
            assert pe.OPTIONAL_HEADER.Magic == 0x10b, "expected a 32-bit server"
            base = pe.OPTIONAL_HEADER.ImageBase
            self.cpu.mem_map(base, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095)
            for section in pe.sections:
                self.cpu.mem_write(base + section.VirtualAddress, section.get_data())
            strings = pe.FILE_HEADER.PointerToSymbolTable + 18 * pe.FILE_HEADER.NumberOfSymbols
            i = 0
            while i < pe.FILE_HEADER.NumberOfSymbols:
                at = pe.FILE_HEADER.PointerToSymbolTable + 18 * i
                raw, value, section, _, _, extra = struct.unpack_from('<8sIhHBB', data, at)
                if raw[:4] == b'\0' * 4:
                    name_at = strings + struct.unpack_from('<I', raw, 4)[0]
                    name = data[name_at:data.index(b'\0', name_at)].decode()
                else:
                    name = raw.split(b'\0')[0].decode()
                if section > 0:
                    self.symbols[name.removeprefix('_')] = base + pe.sections[section - 1].VirtualAddress + value
                i += 1 + extra
        else:
            assert data[:5] == b"\x7fELF\x01", "expected a 32-bit ELF server"
            ph, sh = struct.unpack_from('<II', data, 28)
            pn, sn = struct.unpack_from('<H', data, 44)[0], struct.unpack_from('<H', data, 48)[0]
            segments = [struct.unpack_from('<8I', data, ph + 32 * i) for i in range(pn)]
            end = max(v[2] + v[5] for v in segments if v[0] == 1)
            self.cpu.mem_map(0, (end + 4095) & ~4095)
            for v in segments:
                if v[0] == 1:
                    self.cpu.mem_write(v[2], data[v[1]:v[1] + v[4]])
            sections = [struct.unpack_from('<10I', data, sh + 40 * i) for i in range(sn)]
            for sec in sections:
                if sec[1] != 2:
                    continue
                strings = sections[sec[6]][4]
                for at in range(sec[4], sec[4] + sec[5], sec[9]):
                    name, value, _, _, _, index = struct.unpack_from('<IIIBBH', data, at)
                    if index and name:
                        name_at = strings + name
                        self.symbols[data[name_at:data.index(b'\0', name_at)].decode()] = value
            # Bind imported CRT functions at their PLT entry, without running a dynamic loader.
            strings = sections[struct.unpack_from('<H', data, 50)[0]][4]
            names = {data[strings + sec[0]:data.index(b'\0', strings + sec[0])].decode(): sec for sec in sections}
            if '.rel.plt' in names:
                rel, plt = names['.rel.plt'], names['.plt']
                dynamic = sections[rel[6]]
                strings = sections[dynamic[6]][4]
                for i, at in enumerate(range(rel[4], rel[4] + rel[5], rel[9])):
                    _, info = struct.unpack_from('<II', data, at)
                    name = struct.unpack_from('<I', data, dynamic[4] + (info >> 8) * dynamic[9])[0]
                    name_at = strings + name
                    self.symbols[data[name_at:data.index(b'\0', name_at)].decode()] = plt[3] + 16 * (i + 1)
            # The server's non-PIC restored code uses dynamic PC-relative text relocations.
            # Resolve calls into the same PLT entries that the dependency hooks implement.
            for rel in sections:
                if rel[1] != 9:
                    continue
                dynamic = sections[rel[6]]
                strings = sections[dynamic[6]][4]
                for at in range(rel[4], rel[4] + rel[5], rel[9]):
                    target, info = struct.unpack_from('<II', data, at)
                    kind, index = info & 255, info >> 8
                    if kind not in [1, 2] or not index:
                        continue
                    name = struct.unpack_from('<I', data, dynamic[4] + index * dynamic[9])[0]
                    name_at = strings + name
                    resolved = self.symbols.get(data[name_at:data.index(b'\0', name_at)].decode())
                    if resolved is not None:
                        addend = struct.unpack('<i', self.cpu.mem_read(target, 4))[0]
                        value = resolved + addend - (target if kind == 2 else 0)
                        self.cpu.mem_write(target, struct.pack('<I', value & 0xffffffff))
        self.cpu.mem_map(self.scratch, 0x100000)
        self.cpu.mem_map(0x30000000, 0x10000)
        self.cpu.reg_write(UC_X86_REG_FPCW, 0x27f)
        self.cpu.reg_write(UC_X86_REG_MXCSR, 0x1f80)
        self.physics, self.errors = [], 0
        self.vehicle_fixture = False
        self.vehicle_steps = []
        self.snapshot_fixture = False
        self.snapshot_bytes, self.delta_sources = [], []
        self.allocate = None
        self.info_value = None
        for name in ['memset', 'memcpy', 'strcmp', 'AllocVariable', 'Info_ValueForKey', 'G_RunMissile',
                     'Scr_Error', 'SL_RemoveRefToString', 'floorf', 'AngleVectors', 'G_TraceCapsule', 'VEH_GroundPlant',
                     'VEH_TouchEntities', '_Z15VEH_SetPositionP9gentity_sPKfS2_', 'Sys_Milliseconds',
                     'Com_DPrintf', 'MSG_WriteByte', 'MSG_WriteLong', 'MSG_WriteBit0', 'MSG_WriteEntityIndex',
                     'MSG_ClearLastReferencedEntity', 'MSG_WriteDeltaPlayerstate']:
            address = self.symbols.get(name)
            if address:
                self.cpu.hook_add(UC_HOOK_CODE, self.dependency, begin=address, end=address)
        self.put(self.symbols['gScrVarGlob'], self.scratch)

    def put(self, address, value):
        self.cpu.mem_write(address, struct.pack('<I', value & 0xffffffff))

    def get(self, address):
        return struct.unpack('<I', self.cpu.mem_read(address, 4))[0]

    def put_float(self, address, value):
        self.cpu.mem_write(address, struct.pack('<f', value))

    def get_float(self, address):
        return struct.unpack('<f', self.cpu.mem_read(address, 4))[0]

    def level(self, name, value=None):
        address = self.symbols['level'] + self.layout['level_' + name]
        if value is not None:
            self.put(address, value)
        return self.get(address)

    def call(self, name, *arguments):
        self.put(self.stack, self.stop)
        for i, argument in enumerate(arguments):
            self.put(self.stack + 4 + i * 4, argument)
        self.cpu.reg_write(UC_X86_REG_ESP, self.stack)
        try:
            self.cpu.emu_start(self.symbols[name], self.stop, count=100000)
        except UcError as error:
            ip = self.cpu.reg_read(UC_X86_REG_EIP)
            nearby = max((v, k) for k, v in self.symbols.items() if v <= ip)
            sp = self.cpu.reg_read(UC_X86_REG_ESP)
            raise AssertionError(f'{name}: {error} at {ip:x} ({nearby[1]}+{ip-nearby[0]:x}), stack {sp:x}: {bytes(self.cpu.mem_read(sp, 20)).hex()}') from error
        assert self.cpu.reg_read(UC_X86_REG_EIP) == self.stop, (name, hex(self.cpu.reg_read(UC_X86_REG_EIP)))
        return self.cpu.reg_read(UC_X86_REG_EAX)

    def dependency(self, cpu, address, size, unused):
        stack = cpu.reg_read(UC_X86_REG_ESP)
        if address == self.symbols.get('Info_ValueForKey') and self.info_value is not None:
            cpu.reg_write(UC_X86_REG_EAX, self.info_value)
        elif address == self.symbols.get('AllocVariable') and self.allocate is not None:
            cpu.reg_write(UC_X86_REG_EAX, self.allocate)
        elif address == self.symbols.get('strcmp'):
            stack = cpu.reg_read(UC_X86_REG_ESP)
            left, right = self.get(stack + 4), self.get(stack + 8)
            for i in range(1024):
                a, b = cpu.mem_read(left + i, 1)[0], cpu.mem_read(right + i, 1)[0]
                if a != b or not a:
                    cpu.reg_write(UC_X86_REG_EAX, (a - b) & 0xffffffff)
                    break
            else:
                raise AssertionError('unterminated fixture string')
        elif address == self.symbols.get('memset'):
            stack = cpu.reg_read(UC_X86_REG_ESP)
            dest, value, length = self.get(stack + 4), self.get(stack + 8), self.get(stack + 12)
            cpu.mem_write(dest, bytes([value & 255]) * length)
            cpu.reg_write(UC_X86_REG_EAX, dest)
        elif address == self.symbols.get('memcpy'):
            dest, source, length = self.get(stack + 4), self.get(stack + 8), self.get(stack + 12)
            cpu.mem_write(dest, bytes(cpu.mem_read(source, length)))
            cpu.reg_write(UC_X86_REG_EAX, dest)
        elif self.vehicle_fixture and address == self.symbols.get('floorf'):
            # Flat-airborne fixture has zero yaw; floor(0.5) is the only CRT result needed.
            assert abs(self.get_float(stack + 4) - .5) < 1e-6
            thunk = self.scratch + 0x6000
            cpu.mem_write(thunk, b'\xd9\xee\xc3')  # fldz; ret
            cpu.reg_write(UC_X86_REG_EIP, thunk)
            return
        elif self.vehicle_fixture and address == self.symbols.get('G_TraceCapsule'):
            trace = self.get(stack + 4)
            cpu.mem_write(trace, b'\0' * 44)
            self.put_float(trace, 1)  # no collision in empty space
            self.put_float(trace + 12, 1)
        elif self.vehicle_fixture and address == self.symbols.get('AngleVectors'):
            angles = self.get(stack + 4)
            assert bytes(cpu.mem_read(angles, 12)) == b'\0' * 12
            for at, vector in [(8, (1, 0, 0)), (12, (0, -1, 0)), (16, (0, 0, 1))]:
                output = self.get(stack + at)
                if output:
                    cpu.mem_write(output, struct.pack('<3f', *vector))
        elif self.vehicle_fixture and address == self.symbols.get('VEH_TouchEntities'):
            self.vehicle_steps.append(self.get_float(stack + 8))
        elif self.vehicle_fixture and address in [self.symbols.get(name) for name in
                ['VEH_GroundPlant', '_Z15VEH_SetPositionP9gentity_sPKfS2_']]:
            pass  # terrain, touches and world linking are external to the integration under test
        elif self.snapshot_fixture and address == self.symbols.get('Sys_Milliseconds'):
            cpu.reg_write(UC_X86_REG_EAX, 50000)
        elif self.snapshot_fixture and address == self.symbols.get('MSG_WriteByte'):
            self.snapshot_bytes.append(self.get(stack + 8))
        elif self.snapshot_fixture and address == self.symbols.get('MSG_WriteDeltaPlayerstate'):
            self.delta_sources.append(self.get(stack + 16))
        elif self.snapshot_fixture and address in [self.symbols.get(name) for name in
                ['Com_DPrintf', 'MSG_WriteLong', 'MSG_WriteBit0', 'MSG_WriteEntityIndex', 'MSG_ClearLastReferencedEntity']]:
            pass
        elif address == self.symbols['G_RunMissile']:
            seconds = struct.unpack('<f', cpu.mem_read(self.symbols['missile_frametime'], 4))[0]
            self.physics.append((self.level('time'), self.level('previous'), self.level('frame_time'), seconds))
        elif address == self.symbols['Scr_Error']:
            self.errors += 1
        elif address == self.symbols.get("SL_RemoveRefToString"):
            pass
        else:
            return
        stack = cpu.reg_read(UC_X86_REG_ESP)
        cpu.reg_write(UC_X86_REG_EIP, self.get(stack))
        cpu.reg_write(UC_X86_REG_ESP, stack + 4)

    def thread(self, thread_id, parent=None):
        self.put(self.scratch + 16 * (thread_id + 1) + 8, 0x6e if parent is None else (parent << 8) | 0x71)

    def resume(self, root):
        kind = self.get(self.scratch + 16 * (root + 1) + 8) & 31
        if kind == 16:
            self.call('Scr_ClearWaitTime', root)
        elif kind == 15:
            self.call('Scr_RemoveThreadNotifyName', root)

    def wait(self, seconds, now, thread_id=100, integer=False):
        self.put(self.symbols['gFs'] + 4, thread_id)
        self.put(self.symbols['gScrVarPub'] + 20, now & 0xffffff)
        root = self.call('GetStartLocalId', thread_id)
        self.resume(root)
        value = self.scratch + 0x9000
        self.cpu.mem_write(value, struct.pack('<iI', seconds, 6) if integer else struct.pack('<fI', seconds, 5))
        ticks = self.call('VM_CalcWaitTime', value)
        if ticks < 0xffffff:
            self.call('Scr_SetThreadWaitTime', root, (now + ticks) & 0xffffff)
        return ticks


def run(path, layout):
    n = Native(path, layout)
    count = 0
    n.thread(100)
    n.thread(101, 100)
    for fps in [20, 100, 125]:  # 125 exercises the VM rounding regression, not supported server admission.
        n.level('framerate', fps)
        for seconds, repeats in [(.05, 200), (1, 60), (60, 1)]:
            n.thread(100)
            now = 0
            for _ in range(repeats):
                now += n.wait(seconds, now, 101)
                count += 1
            assert abs(now * 1000 / fps - seconds * repeats * 1000) <= 500 / fps
        assert n.wait(60, 0, integer=True) == fps * 60
    n.level('framerate', 125)
    n.thread(100)
    assert n.wait(.05, 100) == 6 and n.wait(.05, 106) == 7
    n.thread(100)
    assert n.wait(.05, 100) == 6
    n.put(n.scratch + 16 * 101 + 8, 0)
    n.allocate = 100
    assert n.call('AllocThread', 0) == 100
    n.allocate = None
    assert n.wait(.05, 106) == 6  # reused ID must not inherit the old .25-tick remainder
    n.thread(100)
    assert n.wait(.05, 100) == 6
    n.resume(100)
    # A notify string id can collide exactly with the previous timed wake.
    n.call('Scr_SetThreadNotifyName', 100, 106)
    assert n.wait(.05, 106) == 6
    assert n.wait(.05, 114) == 6  # late resume resets carry
    n.thread(100)
    assert n.wait(.05, 200) == 6
    n.resume(100)
    n.put(n.symbols['gScrVarPub'] + 20, 206)
    n.call('Scr_SetThreadWaitTime', 100, 206)  # actual waittillframeend scheduler path
    assert n.wait(.05, 206) == 6
    n.thread(100)
    assert n.wait(.05, 300) == 6
    assert n.wait(0, 306) == 0
    assert n.wait(.05, 306) == 6
    n.thread(100)
    assert n.wait(.05, 0xfffffd) == 6
    assert n.wait(.05, 3) == 7
    # Negative controls: priorWake alone cannot distinguish these two scheduler paths.
    reset = n.symbols['VM_ResetWaitPhase']
    original = bytes(n.cpu.mem_read(reset, 1))
    n.cpu.mem_write(reset, b'\xc3')
    n.cpu.ctl_remove_cache(reset, reset + 32)
    try:
        n.thread(100)
        assert n.wait(.05, 500) == 6
        n.resume(100)
        n.call('Scr_SetThreadNotifyName', 100, 506)
        assert n.wait(.05, 506) == 7  # wrong carry without the event reset
        n.thread(100)
        assert n.wait(.05, 600) == 6
        n.resume(100)
        n.put(n.symbols['gScrVarPub'] + 20, 606)
        n.call('Scr_SetThreadWaitTime', 100, 606)
        assert n.wait(.05, 606) == 7  # wrong carry without the frame-end reset
    finally:
        n.cpu.mem_write(reset, original)
        n.cpu.ctl_remove_cache(reset, reset + 32)
    for value in [-1, float('nan'), float('inf')]:
        assert n.wait(value, 0) == 1
    assert n.errors == 3

    entity = n.scratch + 0x10000
    n.level('entities', entity)
    for fps in [20, 100]:
        n.call('G_InitSomeVariables', fps)
        n.cpu.mem_write(entity, b'\0' * layout['entity_size'])
        n.put(entity + layout['entity_type'], 4)
        n.put(entity + layout['entity_inuse'], 1)
        n.put(entity + layout['entity_parent'], 1)  # owner credit must not bypass free-missile pacing
        n.put(entity + layout['entity_throw_time'], 1000)
        n.physics.clear()
        step = 1000 // fps
        for frame, time in enumerate(range(1000 + step, 1501, step), 1):
            n.level('frame_number', frame)
            n.level('time', time)
            n.level('previous', time - step)
            n.level('frame_time', step)
            n.call('G_RunFrameForEntity', entity)
            assert n.level('previous') == time - step and n.level('frame_time') == step
        assert len(n.physics) == 10, (fps, n.physics)
        assert [v[0] for v in n.physics] == list(range(1050, 1501, 50))
        for now, previous, duration, seconds in n.physics:
            assert now - previous == duration == 50 and abs(seconds - .05) < 1e-7
        # Entity generation changes start a new missile's timeline, not its predecessor's.
        n.put(entity + layout['entity_use_count'], 1)
        n.put(entity + layout['entity_throw_time'], 1500)
        n.level('frame_number', 100)
        n.level('time', 1510)
        n.call('G_RunFrameForEntity', entity)
        if fps == 100:
            assert len(n.physics) == 10
    n.call('G_InitSomeVariables', 100)
    parent, tag = entity + layout['entity_size'], n.scratch + 0x8000
    n.cpu.mem_write(entity, b'\0' * (2 * layout['entity_size']))
    n.put(entity + layout['entity_type'], 4)
    n.put(entity + layout['entity_inuse'], 1)
    n.put(entity + layout['entity_tag'], tag)
    n.put(tag, parent)
    n.physics.clear()
    for frame in range(1, 11):
        n.level('frame_number', frame)
        n.put(parent + layout['entity_processed'], frame)
        n.level('time', 2000 + frame * 10)
        n.level('previous', 1990 + frame * 10)
        n.level('frame_time', 10)
        n.call('G_RunFrameForEntity', entity)
    assert len(n.physics) == 10
    for now, previous, duration, seconds in n.physics:
        assert now - previous == duration == 10 and abs(seconds - .01) < 1e-7
    # Native vehicle thinks keep the same 50 ms integration while the world runs at either rate.
    # Only terrain/touch/world-link dependencies are fixtures; acceleration and slide integration execute.
    vehicle = n.scratch + 0x20000
    n.vehicle_fixture = True
    velocities = []
    for fps in [20, 100]:
        n.call('G_InitSomeVariables', fps)
        n.cpu.mem_write(entity, b'\0' * layout['entity_size'])
        n.cpu.mem_write(vehicle, b'\0' * 0x400)
        n.put(entity + 0x164, vehicle)
        n.put(vehicle + 0x1c0, 9)
        for offset in [0x2f4, 0x300, 0x30c]:
            n.put(vehicle + offset, 1023)
        n.put(vehicle + 0x30c, 0)
        n.put(vehicle + 0x1d4, 100)
        n.put(entity + 0xc4, 1)
        n.put(n.symbols['g_entities'] + 0x15c, n.scratch + 0x30000)
        for i, (name, value) in enumerate([('vehTestHorsepower', 0), ('vehTestWeight', 1),
                                         ('vehTestMaxMPH', 100), ('vehDebugServer', 0)]):
            cvar = n.scratch + 0x7800 + i * 32
            n.put(n.symbols[name], cvar)
            n.put_float(cvar + 12, value)
        n.level('frame_time', 1000 // fps)
        n.put_float(vehicle + 0x138, 100)
        n.level('time', 1000)
        n.call('G_VehEntHandler_Think', entity)
        velocities.append(n.get_float(vehicle + 0x140))
        assert n.get(entity + 0x19c) == 1050
        assert n.get(vehicle + 0x1d4) == 50  # the vehicle-local cooldown shares the 50 ms cadence
        assert abs(n.vehicle_steps[-1] - .05) < 1e-7
        assert n.level('frame_time') == 1000 // fps
        assert velocities[-1] < -10, (fps, velocities)  # real native gravity/drag integration ran
        assert abs(n.get_float(vehicle + 0xc0) - 5) < .001  # 100 units/s * .05 seconds
    assert velocities[0] == velocities[1], velocities
    n.vehicle_fixture = False

    # A real player, map restart, or bot reusing the slot must never inherit old delta/ack state.
    arena = 0x21000000
    size = layout['snapshot_size'] * 256
    n.cpu.mem_map(arena, (size + 4095) & ~4095)
    n.put(n.symbols['snapshotFrames'], arena)
    n.put(n.symbols['snapshotClientCount'], 1)
    client = n.symbols['svs'] + layout['server_clients']
    n.info_value = n.scratch + 0x7000
    expected = bytearray(layout['snapshot_size'])
    expected[layout['snapshot_ack']:layout['snapshot_ack'] + 4] = b'\xff' * 4
    n.snapshot_fixture = True
    header = n.symbols['svsHeader']
    n.put(header + layout['header_clients'], client)
    n.put(header + layout['header_time'], 50000)
    n.put(header + layout['header_entity_count'], 1000)
    n.put(header + layout['header_client_count'], 1000)
    n.put(client + layout['client_state'], layout['active_state'])
    n.put(client + layout['client_gentity'], entity)
    n.put(client + layout['client_address_type'], layout['ip_address_type'])
    cvar = n.scratch + 0x7900
    n.put(cvar + layout['cvar_integer'], 1)
    n.put(n.symbols['sv_maxclients'], cvar)
    n.put(n.symbols['sv_padPackets'], cvar + 32)
    n.put(cvar + 32 + layout['cvar_integer'], 0)
    for slots, step in [(32, 50), (256, 10)]:
        n.put(n.symbols['snapshotSlotCount'], slots)
        n.put(n.symbols['activeFrameMsec'], step)
        assert n.call('SV_SnapshotSlots') == slots
        for advertised, capacity in [(b'256\0', slots), (b'\0', 32)]:
            n.cpu.mem_write(n.info_value, advertised)
            n.cpu.mem_write(arena, b'\xa5' * size)
            n.call('SV_ResetClientSnapshots', client)
            assert bytes(n.cpu.mem_read(arena, layout['snapshot_size'] * slots)) == bytes(expected) * slots
            assert n.call('SV_ClientDeltaCapacity', client) == capacity
            assert n.call('SV_ClientSnapshot', client, slots + 3) == arena + 3 * layout['snapshot_size']
        # An in-flight pre-restart ack is in ring range, but its cleared snapshot is not a delta base.
        n.put(client + layout['client_outgoing'], 10)
        n.put(client + layout['client_delta'], 9)
        old = arena + 9 * layout['snapshot_size']
        for timestamp, expected_delta in [(0, 0), (49950, 1)]:
            n.put(old + layout['snapshot_time'], timestamp)
            n.snapshot_bytes.clear()
            n.delta_sources.clear()
            n.call('SV_WriteSnapshotToClient', client, n.scratch + 0x8000)
            assert n.snapshot_bytes[1] == expected_delta, (slots, timestamp, n.snapshot_bytes)
            assert bool(n.delta_sources[-1]) == bool(expected_delta)
        # The native ping scan must ignore old values outside the stock-sized time window.
        n.call('SV_ResetClientSnapshots', client)
        n.put(client + layout['client_outgoing'], 1000)
        samples = min(slots, 1600 // step)
        for age in range(1, slots + 1):
            frame = arena + ((1000 - age) & (slots - 1)) * layout['snapshot_size']
            sent = 50000 - age * step
            n.put(frame + layout['snapshot_sent'], sent)
            n.put(frame + layout['snapshot_ack'], sent + (50 if age <= samples else 900))
        n.call('SV_CalcPings')
        assert n.get(client + layout['client_ping']) == 50
        if step == 10:
            newest = arena + ((1000 - 1) & (slots - 1)) * layout['snapshot_size']
            n.put(newest + layout['snapshot_sent'], 48000)
            n.put(newest + layout['snapshot_ack'], 48900)
            n.call('SV_CalcPings')
            assert n.get(client + layout['client_ping']) == 50  # sparse sends cannot lengthen the window
    n.info_value = None
    print(f'{path.name}: {count} compiled waits, lifecycle/error paths, 20/100 Hz missile/vehicle integration, ping and snapshot reset/delta checks passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('layout', type=Path)
    parser.add_argument('servers', type=Path, nargs='+')
    args = parser.parse_args()
    for server in args.servers:
        run(server, json.loads(args.layout.read_text(encoding='utf-8-sig')))
