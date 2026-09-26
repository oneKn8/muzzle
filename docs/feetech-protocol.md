# Feetech STS3215 bus protocol: primary-source reference

Fetched 2026-09-26. I recomputed all 18 example frames in [PROTO]: every checksum and LEN is correct.

**Sources** (`file:line` refers to these revisions)
- **[LR]** huggingface/lerobot @ `e595b79` (main, 2026-09-25): `https://github.com/huggingface/lerobot/blob/e595b7902714ba51f91e47523f66f89c5181b649/src/lerobot/` + `motors/feetech/{tables,feetech}.py`, `motors/{motors_bus,encoding_utils}.py`, `robots/so_follower/{so_follower,config_so_follower}.py`, `robots/utils.py`, `scripts/lerobot_teleoperate.py`
- **[SDK]** PyPI `feetech-servo-sdk` 1.0.0, which LeRobot imports as `scservo_sdk` (pyproject.toml:178 pins `>=1.0.0,<2.0.0`): https://files.pythonhosted.org/packages/5f/8e/c53d6f9a8bf3a86a635b58eeb675723f1b040f1665a0681467756c8989aa/feetech-servo-sdk-1.0.0.tar.gz. The file is `protocol_packet_handler.py` unless another is named.
- **[FTC]** Official C++ SDK: `https://github.com/ftservo/FTServo_Linux/blob/06fd3356dbd7bccd886b5a70d7ae0fccc6c76d38/src/` (`INST.h`, `SCS.cpp`, `SMS_STS.{h,cpp}`). **[FTPy]** ftservo/FTServo_Python @ `54e86a7`
- **[PROTO]** Official Feetech e-manual "SCS通信协议" v251 (2026-06-09): `http://doc.feetech.cn/#/tiaozhunlujingft?srcType=FT-SCS-Protocol-41ad23fe8a244712ba160b93`. Raw JSON: `http://longbos.com:9009/workflow/yf-specification/getBysrcType/<srcType>`
- **[MEM]** Official Feetech e-manual "磁编码STS内存表手册" v181 (2026-08-27), srcType `FT-SMS-STS-emanual-229f4476422d4059abfb1cb0`. This is the doc LR tables.py:40 cites; same API as [PROTO].
- **[PDF]** https://files.seeedstudio.com/wiki/robotics/Actuator/feetech/Communication_Protocol_Manual.pdf (V1.01, 2019). Older; [PROTO] supersedes it.

## 1. Packet layout
- Instruction frame: `FF FF ID LEN INST P1..PN CHK`. Status frame: `FF FF ID LEN ERR D1..DN CHK` [PROTO §2-3; PDF p3].
- `LEN = N+2`: the parameter count plus INST/ERR and CHK. **Total frame = LEN+4.** `CHK = ~(ID+LEN+INST/ERR+ΣP) & 0xFF` [PROTO §2]. SDK code: [SDK:86-90], [FTC SCS.cpp:61-89].
- IDs run 0-253; `0xFE` is broadcast [PROTO §2]. The SDK drops status frames with ID>0xFD, LEN>250 or ERR>0x7F [SDK:121-127].
- ERR = 0 means no error. The bits match reg 65 Status [MEM §3.2]: b0 voltage, b1 magnetic encoder, b2 temperature, b3 current, b5 load; b4, b6 and b7 are undefined. SDK `ERRBIT_*` 1/2/4/8/32 agrees [SDK:17-22]. **There is no checksum-error bit.**
- **Byte order:** 16-bit values are little-endian. Addr 2 `END`=0 [MEM §2.1; PROTO §1]; `PacketHandler(0)` puts the low byte first [SDK packet_handler.py:7-9, scservo_def.py:71-84]; LR uses protocol 0 [LR feetech.py:43,122].
- **Timing:** 8N1 framing, 10 bits/byte, so one byte takes 10 µs at 1 Mbps [PROTO §1].

## 2. Instructions [PROTO §4; codes also FTC INST.h:26-35, FTPy scservo_def.py:8-16]
| Code | Name | Params (LEN) | Reply |
|---|---|---|---|
| 0x01 | PING | none (2) | Yes, even to broadcast. Never broadcast it with >1 servo on the bus |
| 0x02 | READ | addr, len (4) | Yes; reply LEN = len+2, then the data |
| 0x03 | WRITE | addr, data… (N+2) | Yes, if unicast and reg 8 = 1 |
| 0x04 | REG_WRITE | addr, data… | Yes. Buffers the write and sets reg 64 = 1 |
| 0x05 | ACTION | none (2) | Runs the buffered writes. Normally sent to broadcast (`FF FF FE 02 05 FA`), so no reply |
| 0x06 | Param restore | none | Yes. Restores everything except ID; needs EEPROM unlocked (§4.10). [PDF] calls it "RESET to factory"; [FTC] calls it `INST_RECOVERY` |
| 0x08 | Reboot | none | **None, even when unicast.** Takes ~800 ms; turn torque off first |
| 0x09 | Param backup | none | Yes. Needs EEPROM unlocked |
| 0x0A | RESET (state) | none | Yes. Resets servo state and turn count |
| 0x0B | Position cal | none (2): current position becomes mid; or 2-byte target (4) | Yes. STS needs firmware ≥ 3.10 (§4.9) |
| 0x82 | SYNC_READ | ID=FE; addr, len, ID1..IDn (n+4) | **One frame per ID, in request-ID order**, each 6+len bytes. Not every servo supports it (§4.7) |
| 0x83 | SYNC_WRITE | ID=FE; addr, L, {ID, L bytes}×n ((L+1)n+4) | None (§4.6) |

- **Broadcast (0xFE):** only PING gets a reply [PROTO §2]. The SDK skips the receive step for broadcast [SDK:186-189].
- **Reg 8:** 0 means the servo replies only to READ and PING; 1 (default) means it replies to everything [MEM §2.2].
- **Hidden calibration path:** writing `Torque_Enable(40) = 128` also sets the current position to 2048 on STS [MEM addr 40; PROTO §4.9; FTC SMS_STS.cpp:134-137].
- **Example SYNC_READ:** request `FF FF FE 06 82 38 08 01 02 36`. Replies: `FF FF 01 0A 00 00 08 00 00 00 00 79 1E 55`, then `FF FF 02 0A 00 FF 07 00 00 00 00 77 23 53` [PROTO §4.7].
- **Reply parsing differs between SDKs:**
  - LR's SDK reads replies strictly in request order. `readRx` throws away frames whose ID doesn't match [SDK group_sync_read.py:58-74; SDK:262-280] and expects `(6+len)*n` bytes [SDK:445].
  - The official C++ SDK searches the reply buffer by ID [FTC SCS.cpp:372-409].

## 3. STS3215 control table
Names come from [LR tables.py:41-101]; area, default and meaning from [MEM §2]. Regions: 0-4 RO version, 5-39 EPROM, 40-55 SRAM R/W, 56-71 SRAM RO, 80-86 factory.
| Addr (bytes) | LeRobot name | Feetech: area, default, meaning |
|---|---|---|
| 0(1), 1(1), 2(1) | Firmware_Major/Minor_Version, — | RO. Addr 2 is `END`; 0 = little-endian |
| 3(2) | Model_Number (sts3215 = 777, tables.py:243) | RO. Feetech splits it: 3 = servo major version, 4 = minor, 1 byte each |
| 5(1), 6(1) | ID, Baud_Rate | EPROM. Defaults 1 and 0 (1M); ID range 0-253 |
| 7(1) | Return_Delay_Time | EPROM. **[MEM] calls this "reserved"** |
| 8(1) | Response_Status_Level | EPROM, default 1 |
| 9(2), 11(2) | Min/Max_Position_Limit | EPROM, defaults 0/4095 (ranges 0-4094 and 1-4095). Set to 0 for multi-turn |
| 13, 14, 15 (1 each) | Max_Temperature/Max_Voltage/Min_Voltage_Limit | EPROM. Defaults 70 °C / — / 40 (0.1 V units) |
| 16(2) | Max_Torque_Limit | EPROM, default 1000 (0.1 %). Copied into addr 48 at power-up |
| 18(1) | Phase | EPROM bitfield: b0 drive direction, b2 speed unit, b3 meaning of speed 0, **b4 angle feedback (0 = single-turn)**, b7 feedback direction. Feetech says don't modify |
| 19, 20 (1 each) | Unloading_Condition, LED_Alarm_Condition | EPROM. Protection/alarm enable bits, same layout as Status |
| 21, 22, 23 (1 each) | P/D/I_Coefficient | EPROM. Position-loop gains |
| 24(**1**; LR says 2), 25(1) | Minimum_Startup_Force, — | EPROM. 24 = minimum startup torque (0.1 %); 25 = integral limit. LR's 2-byte field overlaps 25 |
| 26, 27 (1 each) | CW/CCW_Dead_Zone | EPROM, default 1 |
| 28(2), 30(1) | Protection_Current, Angular_Resolution | EPROM. Defaults 511 (6.5 mA units) and 1 |
| 31(2) | Homing_Offset | EPROM, default 0. Range 0-8191, sign bit 11 (encoding below) |
| 33(1) | Operating_Mode | EPROM, default 0. 0 = position, 1 = constant speed, 2 = PWM, 3 = step |
| 34, 35, 36 (1 each) | Protective_Torque, Protection_Time, Overload_Torque | EPROM. Hold torque after overload (default 20 %), timer (200 × 10 ms), threshold (80 %) |
| 37, 38, 39 (1 each) | Vel-loop P, Over_Current_Protection_Time, Vel-loop I | EPROM. 38 defaults to 200 × 10 ms |
| 40(1) | Torque_Enable | SRAM, default 0. 0 = off, 1 = on, 2 = damping, **128 = set current position to 2048** |
| 41(1) | Acceleration | SRAM, default 0 (= maximum), units 8.7 °/s² |
| 42(2), 46(2) | Goal_Position, Goal_Velocity | SRAM. Bit 15 = direction. Velocity 0 = maximum |
| 44(2) | Goal_Time | SRAM. Feetech calls it "PWM open-loop speed": default 1000, bit 10 = direction |
| 48(2) | Torque_Limit | SRAM, range 0-1000. Addr 50-54 are undefined |
| 55(1) | Lock | SRAM, **default 1** (see §4) |
| 56(2), 58(2), 60(2) | Present_Position, Present_Velocity, Present_Load | RO. Direction bits: b15, b15, b10 |
| 62, 63, 64, 65, 66 (1 each) | Present_Voltage, Present_Temperature, —, Status, Moving | RO. 64 = REG_WRITE pending flag; 65 uses the §1 bits |
| 67(2), 69(2), 71(2) | —, Present_Current, Goal_Position_2 | RO. 67 = current goal; 69 in 6.5 mA units; **71 is undefined in [MEM]** |
| 80-86 (1 each) | Moving_Velocity_Threshold, DTs, Velocity_Unit_factor, Hts, Maximum_Velocity_Limit, Maximum_Acceleration, Acceleration_Multiplier | **Factory, read-only in [MEM].** 83 is the minimum speed limit, 85 the acceleration limit |

**Sign-magnitude encodings** [LR tables.py:207-216, encoding_utils.py:16-36]:
- Bit 15: Goal_Position, Goal_Velocity, Present_Position, Present_Velocity.
- Bit 10: Present_Load.
- Bit 11: Homing_Offset.
- These match [MEM] and [FTC SMS_STS.cpp:158,191]. The official SDK also treats Present_Current as bit 15 [FTC:240].

**Homing_Offset** [MEM addr 31]:
- Raw 0-2047 means +0..+2047; 2048-4095 means −0..−2047; 4096-6143 means +2048..+4095; 6144-8191 means −2048..−4095.
- So bit 11 is the sign and bit 12 is the high magnitude bit.
- LR encodes only bit 11, so it accepts |offset| ≤ 2047 and raises beyond that.
- LR's model: `Present = Actual − Homing_Offset` [LR feetech.py:278-289].

## 4. Lock (addr 55)
- **Values** [MEM §2.3]: default 1.
  - **Write 0:** the write-lock turns off, and values written to EPROM addresses survive power-off.
  - **Write 1:** the write-lock turns on, and values written to EPROM addresses are **not** kept after power-off.
- **Lock controls persistence, not whether a write is accepted.** [PROTO §4.3] says to set Lock to 0 before changing the ID "or the ID won't be saved at power-off".
- 0x06 restore and 0x09 backup both need the EEPROM unlocked [PROTO §4.10-11].
- **SDK helpers:** `unLockEprom` writes 55←0 and `LockEprom` writes 55←1 [FTC SMS_STS.cpp:124-132].
- **LR pairs Lock with torque:** `disable_torque` writes Torque_Enable←0 then Lock←0; `enable_torque` writes Torque_Enable←1 then Lock←1 [LR feetech.py:291-305].

## 5. What LeRobot's SO-101 follower sends
**Motors and call types.** IDs 1-6 are shoulder_pan, shoulder_lift, elbow_flex, wrist_flex, wrist_roll, gripper; all sts3215 [LR so_follower.py:72-83].
- `write` = unicast WRITE. It waits for the status reply and raises if ERR≠0 [motors_bus.py:1066-1125].
- `sync_write` = SYNC_WRITE with no reply [:1220-1292].
- `read` = READ.

**connect()** [so_follower.py:113-131] runs these steps:
1. **Handshake, per ID:** PING followed by READ(3,2); the SDK's `ping` does both. Then READ(0,1) and READ(1,1) [motors_bus.py:465-502; SDK:208-228; feetech.py:431-448].
2. **`is_calibrated`:** per motor, READ(9,2), READ(11,2), READ(31,2) [feetech.py:227-266].
3. **Calibration, only if step 2 finds a mismatch** [so_follower.py:137-181]:
   - **Using a saved file:** WRITE Homing_Offset, Min_Position_Limit and Max_Position_Limit per motor [feetech.py:268-276]. **Torque and Lock are not touched first.**
   - **Full calibration** [motors_bus.py:754-852]:
     1. `disable_torque`, then Operating_Mode←0 on each motor.
     2. Reset: Homing_Offset←0, Min←0, Max←4095.
     3. SYNC_READ Present_Position, then WRITE Homing_Offset = position − 2047.
     4. Repeat SYNC_READ Present_Position every ~20 ms (5 motors, retry 5) until the user presses Enter.
     5. `write_calibration`, with wrist_roll set to 0-4095.
4. **configure()** [so_follower.py:183-195], inside `torque_disabled()` [motors_bus.py:676-691]. Every write here is a unicast WRITE:
   1. Per motor: Torque_Enable←0, then Lock←0.
   2. Per motor [feetech.py:209-225]: Return_Delay_Time(7)←0, Maximum_Acceleration(85)←254, Acceleration(41)←254. Then READ Phase(18); if bit 4 is set, WRITE Phase & ~0x10.
   3. Per motor: Operating_Mode(33)←0, P(21)←16, I(23)←0, D(22)←32 [config:45-47].
   4. Gripper only: Max_Torque_Limit(16)←500, Protection_Current(28)←250, Overload_Torque(36)←25.
   5. Per motor: Torque_Enable←1, then Lock←1.

   These EPROM writes all happen with Lock=0, so **every connect() writes them permanently.**

**Every control step** (teleop default 60 fps) [lerobot_teleoperate.py:140,193-219]:
- **Observation:** SYNC_READ Present_Position(56,2) for all 6 IDs, up to 3 tries (`num_read_retries=2`) [so_follower.py:203-207; config:53].
- **Action:** one SYNC_WRITE Goal_Position(42, L=2) using bit-15 sign-magnitude [so_follower.py:243-254].
  - If `max_relative_target` is set, LR first does an extra SYNC_READ and clamps each goal to present ± cap, in normalized units [robots/utils.py:93-128]. It is None by default [config:36].
- **Goals are not clamped to the calibrated range for body joints.** They use DEGREES (`use_degrees=True` by default [config:42]), and that branch of `_unnormalize` has **no clamp** [motors_bus.py:904-907]. Only the gripper's RANGE_0_100 branch clamps [:900-903]. So the host can send goals outside the calibrated range.

**disconnect()** (on by default): per motor, Torque_Enable←0 then Lock←0, with 5 retries [motors_bus.py:546-562]. **This leaves Lock=0.**

**setup_motors** (one-time): WRITE ID, then Baud_Rate←0 [motors_bus.py:589-631].

## 6. Bad checksum: UNVERIFIED
No primary source says what a servo does with a corrupt instruction.
- [PROTO §1] says only that the servo matching the ID "receives the command completely" and replies.
- ERR has no checksum bit, unlike Dynamixel 1.0.
- On the host side, a bad status frame shows up as `COMM_RX_CORRUPT` [SDK:145-155] or `ERR_CRC_CMP` [FTC SCS.cpp:205-214].

## 7. Units and baud rate
- **Position:** 4096 ticks/rev [LR tables.py:186-194], range 0-4095, 0.087° per tick [MEM]. Phase b4=0 gives single-turn 0-4095, and LR clears b4 [feetech.py:219-225].
- **Defaults:** 1,000,000 bps, ID 1, 8N1 [MEM §1; LR feetech.py:44].
- **Baud codes (addr 6)** [MEM §2.2; FTC INST.h:38-49; ftservo-wiki docs/en/reference/protocol.md]: 0=1M, 1=500k, 2=250k, 3=128k, 4=115200, 5=76800, 6=57600, 7=38400.
  - **LR has codes 5-7 wrong** (57600/38400/19200) [LR tables.py:154-163]. LR only ever writes code 0.

## UNVERIFIED (bench-test before relying on these)
1. Whether a servo silently drops a bad-checksum instruction, or replies somehow.
2. Whether an EPROM-area write made while Lock=1 takes effect immediately and lasts until power-off. The manual's wording implies yes.
3. What addr 7 does on STS3215. LR calls it Return_Delay_Time with a default of 500 µs; [MEM] says "reserved".
4. Whether writes to factory addresses 80-86 are accepted or applied. LR writes 85←254; [MEM] marks it read-only.
5. The gap between SYNC_READ replies, and the delay before any status reply. No source gives these timings.
6. Whether STS3215 firmware supports 0x06, 0x08, 0x09 and 0x0A. [PROTO] says support depends on the model.
7. What the servo does with a goal outside Min/Max (clamp, reject, or set ERR b1), and with a write that crosses a field boundary (e.g. LR's 2-byte write at 24 spilling into 25).
