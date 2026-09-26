# muzzle: mechanism prior-art check (claims A, B, C)

Checked 2026-09-26. About 30 web searches plus GitHub repo search across papers, patents, vendor manuals
and forums. Zero hits is never read as novelty: every verdict names its nearest neighbors. Full text read
for Taubig 2012, YASIR 2008, Krasowski 2023, the Goldman patent and the FANUC manual. The MDPI Dec 2025
paper returned 403 and was not re-read.

## Claim A: asynchronous servos sweep the whole box [q, g], so endpoint and segment checks are unsound

**Verdict: EXISTS PARTIALLY.** The effect is well known in practice, and checking the whole joint box
conservatively is published. No source was found that states "reachable set = box, so endpoint filters are
unsound" for bus servos.

- Taubig, Baeuml, Frese, "Real-time Continuous Collision Detection for Mobile Manipulators" (DFKI/DLR 2012; extends their IROS 2011 swept-volume paper). https://elib.dlr.de/78876/1/T%C3%A4ubig2012.pdf . **Closest.** It takes per-joint intervals Q=[q0,q1] (where each joint may stop while braking) and conservatively bounds the swept volume of their Cartesian product, which is exactly a check on the joint box. It is motivated by braking, not by unsynchronized position servos, and never compares itself with endpoint checking.
- FANUC Series 0i-MD User's Manual B-64304EN-2. https://cncedu.hu/wp-content/uploads/2020/10/Fanuc-0i-Mate-MDUsers-Manual.pdf . With G00 and parameter 1401 LRP=0, "positioning is performed independently along each axis", and a WARNING says nonlinear-interpolation positioning needs the tool path confirmed because a collision can happen. This is the machinists' well-known "dog-leg rapid". It is the same effect, but the path is one known dogleg (axis speeds are known), not a whole box.
- Industrial PTP guidance: joint-space PTP paths are "unpredictable", so check the whole move (https://www.solisplc.com/tutorials/industrial-robot-motion-types). On hobby arms, LeRobot `max_relative_target` (https://github.com/huggingface/lerobot/issues/1483) caps the per-joint step. That shrinks the box but checks no Cartesian constraint. LeRobot `end_effector_bounds` clip only the commanded endpoint.

**Residual gap:** (1) a stated, tight result for bus servos: when each servo's speed and acceleration are unknown, every point in the box can be reached, so endpoint and straight-segment filters are unsound for Cartesian keep-outs. (2) A measured corner breach on an SO-100-class arm. **Caveat that weakens the claim:** the box bounds the motion only if every servo moves monotonically toward its goal. Position-loop overshoot and stale q (the arm keeps moving between the read and the command) can leave the box. The sound set is the box built from measured q, inflated by an overshoot and latency margin.

## Claim B: cut-through UART firewall that clamps bytes in flight, recomputes the checksum, and kills by poisoning it

**Verdict: EXISTS PARTIALLY. The kill half EXISTS AS-IS on serial links.**

- Tsang and Smith, "YASIR: A Low-Latency, High-Integrity Security Retrofit for Legacy SCADA Systems", IFIP SEC 2008. https://dl.ifip.org/db/conf/sec/sec2008/TsangS08.pdf (live server returned 503; readable via web.archive.org). **Kills the kill-mechanism novelty.** It is a serial bump-in-the-wire for Modbus/DNP3 at 9600-115200 baud. The receiver "relays every byte ... with a delay of 14 byte times", and if the HMAC fails "R manipulates the last byte to cause the conformance checks at D to fail". It chooses err as any byte that differs from the correct CRC's last byte. Differences from muzzle: the decision is authentication, not a safety policy; it holds 14 bytes, not 1-2; it never rewrites values.
- Goldman Sachs, US10708394B1 / US11172056B2, "Dynamically configurable network gateway" (priority 2020-02-18). https://patents.google.com/patent/US11172056B2/en . A cut-through pre-trade risk gateway. When validation fails, it modifies the not-yet-sent TCP payload so the TCP checksum fails at the exchange, and "recalculate[s] the FCS as the frame data is forwarded" to account for its own change. That covers modify in flight, recompute over the emitted bytes, and poison to kill, all on Ethernet/TCP. Related: CRC stomping in cut-through switches (https://www.dell.com/support/kbdoc/en-us/000205065/what-is-crc-stomping) and the HFT trick of corrupting the FCS mid-frame (https://libfpga.com/blog/fpgas-for-hft).
- Serial and servo side: Ursescu/modbus-firewall (ESP32/AVR, deep packet inspection via the freemodbus stack, so store-and-forward): https://github.com/Ursescu/modbus-firewall . MDPI Electronics 14(24):4909 (2025) is an FPGA Trojan man-in-the-middle on a Dynamixel bus, an attack paper already in your map. GitHub searches for dynamixel/feetech/servo/uart firewall or proxy found no cut-through clamping servo firewall.

**Residual gap:** clamping at the value level (rewriting goal bytes to a policy bound, not only killing the packet) with a 1-2 byte hold on a half-duplex servo bus, driven by a safety policy rather than authentication. This transplants YASIR plus Goldman into a new domain, so claim it as an engineering combination, not as a new mechanism. **Hazard found while checking:** Dynamixel Protocol 2.0 byte-stuffs FF FF FD into FF FF FD FD, and both LENGTH and the CRC count the stuffed byte (https://emanual.robotis.com/docs/en/dxl/protocol2/). A clamped byte can create or destroy a stuffing trigger after LENGTH has already gone out on the wire. The clamp must pick values that never form the pattern, or else take the kill path. Feetech STS/SCS (Protocol-1 style, 8-bit sum checksum) has no stuffing.

## Claim C: precomputed certified per-joint box that turns a coupled Cartesian constraint into independent streaming clamps

**Verdict: EXISTS PARTIALLY.** Each ingredient is published. The combination and its purpose were not found.

- Krasowski, Thumm, et al., "Provably Safe Reinforcement Learning: Conceptual Analysis, Survey, and Benchmarking", TMLR 2023. https://arxiv.org/abs/2205.06750 . It represents the safe action set as an axis-aligned box and enforces it element-wise ("continuous action masking"), and notes that the box under-approximates the safe set. This is the same "a box makes enforcement separable" move, with no forward-kinematics certification and no streaming motive.
- Jaulin, "Path Planning Using Intervals and Graphs", Reliable Computing 2001. https://www.ensta-bretagne.fr/jaulin/paper_cameleon.pdf . With Merlet's interval-analysis robotics work, it certifies C-space boxes collision-free using interval inclusion functions of the kinematics (SIVIA subpavings). Same certifier, used for planning, not for clamping individual joints.
- Amice et al., C-IRIS (https://arxiv.org/abs/2205.03690), and Dai et al. 2023 (https://arxiv.org/abs/2302.12219). Both give certified collision-free C-space regions, but as polytopes: the rows stay coupled, so they are not per-joint clamps, and the SOS/SDP certification is too heavy for an ESP32. Taubig 2012 (above) also works as a real-time box certifier.

**Residual gap:** (1) choosing a box shape *because* the enforcement point gets joint goals one at a time inside a SYNC_WRITE and must decide byte by byte. (2) **The A-C closure argument:** if the measured q and every clamped goal g lie in a certified box B, then the whole asynchronous reachable set box(q, g) lies inside B. So per-joint clamping is sound against unsynchronized motion, while a polytope membership check or an endpoint check is not. This link is the load-bearing piece and it was not found anywhere. Proposing the box from linearized barrier rows is a standard heuristic and carries no priority.

## Bottom line

- **A:** known in substance (Taubig's interval sweep, the CNC dog-leg rapid). Present it as an applied observation backed by a measured breach, not as a discovery.
- **B:** the kill mechanism is YASIR (2008), so cite it. What is new is policy-driven clamping in flight on a servo bus, including the handling of Protocol 2.0 byte stuffing.
- **C plus the A-C closure** (the asynchronous box sweep stays inside a certified box, so byte-level clamps are sound) is the most defensible new claim, and it survived this sweep.
