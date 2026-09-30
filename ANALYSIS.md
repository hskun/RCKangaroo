# RCKangaroo 项目代码分析

> 分析对象：RCKangaroo v4.0（RetiredCoder，GPLv3）
> 用途：用 GPU 求解 secp256k1 椭圆曲线离散对数问题（ECDLP）的 SOTA v2 Kangaroo 方法实现

---

## 一、项目概述

**RCKangaroo v4.0** 是一个用 CUDA GPU 求解 ECDLP 的开源工具，实现了改进型 Pollard-Kangaroo（袋鼠）算法（作者称为 SOTA v2）。典型用途是破解 Bitcoin puzzle（已知公钥落在某个小区间内，求私钥）。

- K = 1.15（经典方法 K ≈ 2.1），所需操作数与 DP 内存均减少约 1.8 倍
- 速度：RTX 4090 约 14.5 GH/s，RTX 5090 约 19.3 GH/s（turbo 汇编 kernel）
- 支持 32–170 bit 区间，Windows / Linux 双平台
- 限制：无网络、无 DP 保存/加载等高级功能（README 明示）

## 二、文件结构与职责

| 文件 | 职责 |
|---|---|
| `RCKangaroo.cpp` | 主程序：命令行解析、GPU 初始化、求解主循环 `SolvePoint`、CPU 端 DP 碰撞检测 `CheckNewPoints` / `Collision_SOTA`、三种模式（MAIN / BENCH / TAMES 生成） |
| `GpuKang.cpp/.h` | 每张 GPU 一个 `RCGpuKang` 对象：显存分配、跳跃表上传、主执行循环 `Execute`（A→B→C 三个 kernel 循环调度）、速度统计 |
| `RCGpuCore.cu` | CUDA kernel 源码（旧卡回退路径）：`KernelGen`（生成袋鼠起点）、`KernelA`（主跳跃）、`KernelB`（距离累计+环检测）、`KernelC`（环逃逸跳跃） |
| `RCGpuUtils.h` | PTX 内联汇编宏 + secp256k1 域运算（`AddModP/SubModP/MulModP/SqrModP/InvModP`，Montgomery 乘法） |
| `Ec.cpp/.h` | CPU 端 256-bit 大整数与椭圆曲线运算（点加、倍点、`MultiplyG`、模逆、模开方），用于准备数据和最终验证 |
| `utils.cpp/.h` | `TFastBase`：以 x 坐标前 3 字节为索引的三级（256³）有序 DP 数据库；`MemPool` 内存池；跨平台线程/锁封装 |
| `CallCubin.cpp/.h` | 通过 CUDA Driver API 加载预编译 cubin 并启动 kernel（turbo 汇编 kernel 的调用层） |
| `kernel_sm89.cubin` / `kernel_sm120.cubin` | 纯汇编（作者自研 RCAsm）编写的 turbo kernel，分别针对 4xxx（sm_89）和 5xxx（sm_120）卡 |
| `*.asm` | cubin 的汇编源码（`main.asm`、`mod_mul.asm`、`mod_inv.asm`、`mod_sub.asm`、`fuse.asm`、`newKernelB.asm`） |
| `CMakeLists.txt` / `.sln/.vcxproj` | Linux（CMake+nvcc）与 Windows（MSVC）构建 |

## 三、核心算法流程

### 3.1 袋鼠方法框架（SOTA v2）

- 区间 `[start, start+2^range)` 内求私钥。总操作数约 `1.15·2^(range/2)`，远优于经典 3-way 袋鼠的 `2.1·2^(range/2)`。
- 袋鼠分两类：1/3 是 TAME（从已知距离出发），2/3 是 WILD（从待解点 P 出发）。SOTA 方法中 wild-wild 碰撞也能恢复私钥（`Collision_SOTA` 中 `TameType != TAME` 分支：距离差减半处理）。
- 主程序先把目标点平移：`PntToSolve = PubKey - start·G + 2^(range-5)·G`（`x32` 偏移用于 "smooth edges"），解出后再还原。

### 3.2 三套跳跃表（`SolvePoint` 中生成，`JMP_CNT=512`）

- `EcJumps1`：正常跳跃，距离约 `2^(range/2+3)` 的随机偶数
- `EcJumps2`：大跳跃（约 `2^(range-10)`），用于逃离 L1S2 短环
- `EcJumps3`：更大的跳跃，用于逃离 >2 级的环

### 3.3 GPU 每轮迭代（`RCGpuKang::Execute`，每轮 `STEP_CNT=1000` 步）

- **KernelA（主跳跃）**：每线程管理 `PNT_GROUP_CNT=24` 个点。关键优化是**批量模逆**（Montgomery trick）：24 个点的 `(x - jmp_x)` 连乘后只做一次 `InvModP`，再反推每个点的逆元，使每次点加只花约 1/24 次模逆。同时检测 DP（x 低 DP 位为 0 的点写入 `DPTable`）、检测 L1S2 环（下一跳与当前跳相同则切换到大跳跃表）、记录每步的跳跃索引 `jlist` 和最后 `MD_LEN=10` 个点。
- **KernelB（距离跟踪+环检测）**：重放 `jlist`，累加 192-bit 距离（`Add192to192`），用环形表（`LoopTable`，按 64-bit 距离比较，误判率约 2^-60）检测更长的环；对带 `DP_FLAG` 的步生成 DP 记录（`BuildDP`）写入 `DPs_out`。
- **KernelC（环逃逸）**：对环住的袋鼠从 `LastPnts` 恢复位置，执行一次 Jump3 跳出环。

### 3.4 CPU 端碰撞检测（`CheckNewPoints`）

GPU 上报的 DP 记录（x 前 12 字节 + 距离 22 字节 + 类型）插入 `TFastBase`；若同 x 已存在则发生 tame/wild 碰撞，用距离差 `t - w`（及其相反数）试算 `d·G == PntToSolve` 验证，成功即得私钥。

### 3.5 Turbo 汇编 kernel（v4.0 核心卖点）

对 sm_89/sm_120 卡，`Asm_CallGpuKernelAB` 用 cubin 中的汇编 KernelA/B 替代 CUDA 版本，并额外启动 `sm_inv_cnt` 个 block 专门做模逆（详见第四节）。袋鼠点数据放在标记为 persisting 的 L2 cache 窗口中（`cudaStreamSetAttribute` + `accessPolicyWindow`），加速主循环访问。

### 3.6 Tames 持久化

`-tames` 文件可保存/加载 tame DP 数据库（`TFastBase::SaveToFile/LoadFromFile`），`-max` 限制操作数后退出并保存，实现跨多次运行复用。

## 四、汇编 Kernel 的 Mailbox 逆元调度机制（重点分析）

### 4.1 要解决的问题

CUDA 版 KernelA 里，每个线程对 24 个点做一次批量模逆（24 个数连乘 → 1 次 `InvModP` → 反推 24 个逆元）。但 `InvModP`（基于二进制扩展欧几里得，迭代次数依赖数据、寄存器占用高）仍是主循环里最贵、最难调度的部分，还挤占主 kernel 的寄存器预算（`regcnt=255` 已满）。

v4.0 的思路：**把模逆从主循环里彻底剥离**——大部分 SM 跑"生产者"（袋鼠跳跃），少数 SM（`sm_inv_cnt` 个 block，4090 为 mpCnt/32，5090 为 mpCnt/24）专职跑"消费者"（逆元计算），两者通过全局内存中的**队列 + mailbox + ReadyFlag** 三套结构异步通信。这就是 README 所说的 "Triple Montgomery trick"（24→8→4→1 三级聚合）。

### 4.2 共享内存布局（Inv 通信区）

位于 `Kparams.L2` 尾部（`InvBase = L2Base + 3*PartStride`，见 `main.asm:70`），主机端 `GpuKang.cpp` 的 `Inv_DataSize` 注释与汇编中的偏移一一对应：

| 偏移 | 结构 | 作用 |
|---|---|---|
| `+0x00` | head 计数器 | 消费者 warp 用 `ATOMG.ADD` 领取下一个批次（32 个槽为一批） |
| `+0x80` | tail 计数器 | 生产者 warp 用 `ATOMG.ADD` 领取一个队列槽位 |
| `+0x100` | ProducerCnt | 完成全部迭代的生产者 warp 数（用于停机判断） |
| `+0x400` | 每 warp 迭代计数器 | 每 warp 每轮 +1（调试用） |
| `+0x2400` | 队列（32K × 4B 环形） | 每格存 `(generation << 12) \| (CID+1)`，即"哪个 warp 的第几代请求" |
| `+0x2400+128KB` | OUT mailbox（2048 × 128B） | 每 warp 4 个 32B 槽，生产者写入待求逆的 4 个数 |
| `+0x2400+256KB` | IN mailbox（2048 × 128B） | 消费者写回 4 个逆元结果 |
| `+0x2400+384KB` | ReadyFlag（2048 × 4B） | 每 warp 一个标志：bit0=结果就绪，0x100=停止标志 |

整个区域位于被标记为 **persisting L2** 的窗口内，且所有访问都用 `.STRONG_GPU` 强一致性语义 + `MEMBAR.SC.GPU` 保证跨 SM 可见性顺序。

### 4.3 生产者侧（普通 block）

KernelA 启动 `BlockCnt + sm_inv_cnt` 个 block，`BlockID >= BlockCnt` 的走 `.inv_sm_begin`，其余走主循环（`main.asm:79-80`）。

**三级聚合，8→1（`Send_ToInv`，main.asm:551-616）**：

1. 每线程对半组 12 个点已算好连乘积 `TmpEightB`（第一级 Montgomery，在 `Calc_ToInv` 内完成，12→1）。
2. warp 内再用 `SHFL.BFLY`（蝶形交换 xor 1/2/4）做三级配对乘法：lane 两两相乘 → 4 个一组 → 8 个一组。最终每个 8-lane 组只剩 lane0/8/16/24 各持有一个聚合值。至此 **24×8=192 个点的逆元被压缩成 4 个数**。
3. lane0 对本 warp 取队列槽：`ATOMG.ADD [InvBase+0x80]` 得 tail，`tail>>15` 是代数（generation），`tail & 32767` 是环形下标。
4. 4 个 8-lane 组长把聚合值写入本 warp 的 OUT mailbox（每 warp 128B = 4×32B）。
5. `MEMBAR.SC.GPU` 后，lane0 把 `(gen<<12)|(CID+1)` 写入队列槽——**先写数据、membar、再发布槽位**，保证消费者看到槽位时 mailbox 一定已填好（发布-订阅顺序）。

**等待结果（`Recv_Inv`，main.asm:618-706）**：

1. lane0 轮询本 warp 的 ReadyFlag（`LDG.STRONG_GPU` 自旋），非零即结果就绪；通过 `SHFL.IDX` 广播给全 warp。
2. 若 ReadyFlag ≥ 0x100，说明消费者已停机（所有生产者即将结束），此时**降级为本地计算**：跳 `.label_local_inv` 自己跑 `InvMod256`——保证尾部不会死锁，也不需要消费者等最后一个请求。
3. 正常情况下 `RED.AND` 清 ReadyFlag（保留 stop 位），从 IN mailbox 读回 4 个逆元。
4. **三级展开（1→8）**：用 `SHFL.IDX` 把 4 个逆元路由回各 8-lane 组，再做三次配对乘法反推，恢复出每线程的逆元 `invNine`。

**流水线重叠**：主循环把一次迭代拆成两半组交替进行——`Calc_ToInv B`（算下一半组的连乘积）→ `Recv_Inv A`（收上一半组的逆元）→ `Send_ToInv B` → `Calc_NewPoints A`（用收到的逆元算新点）。求逆的往返延迟完全被本线程自己的乘法工作掩盖，这就是 README 说的 "inverse calculation is masked completely and excluded from the main loop"。

### 4.4 消费者侧（INV block，main.asm:971-1058）

每个 INV block 的每个 warp 循环：

1. **领批次**：lane0 `ATOMG.ADD [InvBase+0x00]` 取 head，head>>10 为代数，低 10 位为批次号（每批 32 个槽 = 1024 批 × 32 = 32K 环形队列）。
2. **等齐 32 槽**：32 个 lane 各自轮询队列中一个槽，要求 `CID+1 != 0` 且 generation 匹配（防止环形复用时读到上一代残留）；32 个 lane 用 `PLOP3` 汇聚成 warp 级一致条件。等齐期间每轮检查 `ProducerCnt > uStopThr` 则跳 `.inv_end` 停机。
3. **立即释放槽位**：读出 CID 后即可被生产者复用（生产者侧的 tail 环形覆盖由代数值保护）。
4. **第二级聚合 4→1**：从 32 个 warp 的 OUT mailbox 各读 4 个数（每 lane 负责一个 warp），warp 内每 lane 对 4 个数做连乘（`C2=v0*v1; C3=C2*v2; C4=C3*v3`）。
5. **求逆**：每 lane 调用一次 `InvMod256`——**一次逆元服务 4×192=768 个点**。
6. **展开 1→4**：6 次乘法反推出 4 个各自的逆元。
7. **回写**：结果写入对应 warp 的 IN mailbox，`MEMBAR.SC.GPU` 后 `RED.OR` 置对应 ReadyFlag 的 bit0（同样先数据后标志）。

### 4.5 停机协议

- 生产者 warp 完成全部 `STEP_CNT×2` 次半迭代后，`RED.ADD [InvBase+0x100]` 给 ProducerCnt +1，然后正常退出（保存状态、EXIT）。
- 消费者在等槽循环中发现 `ProducerCnt > StopThr`（主机端设为 `0.5*BlockCnt*8`，即过半生产者 warp 已完成）时，认为剩余请求不再值得服务，跳 `.inv_end`：给全部 2048 个 ReadyFlag 写入 0x100 停止标志后 EXIT。
- 还在等待逆元的生产者看到 stop 位后走本地 `InvMod256` 收尾——**优雅降级，无死锁、无遗留请求**。

### 4.6 为什么快

- **逆元吞吐**：一次 `InvMod256` 摊到 768 个点，逆元成本降到约 3%；4090 上只需 1/32 的 SM 专职求逆。
- **零空闲**：生产者用半组交替流水线把 mailbox 往返（数百拍 L2 延迟）藏在自己的乘法下面；消费者永远有满批次可算。
- **无锁化**：全部协调只用 3 个原子计数器 + 每 warp 一个 ReadyFlag，没有全局锁；队列槽 4B、mailbox 128B 对齐，避免 L2 扇区伪共享（注释中特意提到计数器放在 0x00 和 0x80 以避开同一 L2 行）。
- **寄存器解放**：主循环不再需要常驻 `InvMod256` 的状态，255 个寄存器全给跳跃路径用（`InvMod256` 只在 `.label_local_inv` 冷路径内联，且放在 kernel 末尾 "to help i-cache"）。

### 4.7 值得注意的脆弱点

- **代际位宽**：队列环形 32K 槽、代际只有 `tail>>15`（约 17 位），若消费者落后超过一代（32K 个请求）会错配——实际由"生产者必须等自己的 ReadyFlag 才能发下一个请求"这一不变量保证不会超前两代，安全但隐式。
- **自旋轮询无退避**：生产者和消费者都是紧凑自旋 + 强一致 LDG，对 L2 有持续压力；靠 L2 persisting 窗口和访问频率注释（`stall=03 low freq!`）缓解。
- **硬编码常量**：2048 warp、32K 队列、128B mailbox 与 `BlockSize=256`、SM 数上限 256 强耦合，换 BLOCK_SIZE 需要同步改汇编。
- `CallCubin.cpp` 每次 kernel 调用都重新 `cuModuleGetFunction` + `cuFuncSetAttribute`，有少量可避免的开销（但未在热路径上）。

## 五、工程亮点

- **批量模逆 + 仿射坐标点加**：每次跳跃只做乘法和减法，多点共享一次逆元，是 GPU 高吞吐的关键。
- **分层环处理**：L1S2 环在 KernelA 内零成本检测切换跳跃表；长环由 KernelB 用极小开销（每步一次 64-bit 比较）检测，KernelC 逃逸。代码注释里有详尽的环概率统计（如 4090 跑一天 L1S12 只出现约 4 次，论证不值得处理）。
- **内存布局为 coalescing 优化**：x/y/距离按 group 分离存储（`PartStride`），`LOAD_VAL_256`/`SAVE_VAL_256` 用 `int4` 向量读写。
- **DP 开销有量化模型**：`SolvePoint` 中用经验公式 `K = 1.15 + (0.07 + 0.76/√n)/(1+0.30n)` 估算 DP 开销并提示用户调整 `-dp`。

## 六、其他值得注意的问题/风险

- `CallCubin.cpp:40` `int ArgCnt = 1` 硬编码且只传一个参数指针，依赖 kernel 只接收 `TKparams` 一个 by-value 参数——能工作但很脆弱。
- `RCKangaroo.cpp` 中 `strcpy(gTamesFileName, argv[ci])` 等多个命令行参数未做长度检查，存在缓冲区溢出风险（本地工具影响有限）。
- `-gpu` 参数只支持个位数 GPU 索引（`gpus[i]-'0'`），超过 10 张卡无法指定。
- DP 数据库 `TFastBase` 固定在内存中全量构建，无网络/分布式能力；170-bit 区间单卡实际不可行，注释里作者自己也分析了单卡 140-bit 的环风险。
- `CheckNewPoints` 在持锁外单线程处理全部 DP，GPU 极多或 DP 值过小时可能成为瓶颈（有 `MAX_CNT_LIST` 溢出提示）。
- `EcInt` 用 `data[4+1]` 第 5 个字做符号/溢出扩展，负数用补码表示，代码中多处手工符号扩展（如 `memset(...,0xFF,18)`），可读性差但正确性依赖约定。

## 七、总体评价

这是一个**高度优化、目标专一**的研究型实现：算法上（SOTA v2 + 批量逆元 + 分层环处理 + 汇编 kernel）代表了当前 ECDLP 区间求解的 SOTA 工程水平；其跨 SM 生产者-消费者逆元流水线设计精巧——用"24→8（线程内+warp 内 SHFL 聚合）→4（mailbox 装载）→1（消费者 warp 内聚合）"的三级 Montgomery 聚合把模逆密度压到极限，再以代际标记的环形队列 + 每 warp mailbox/ReadyFlag 做异步交接，并带有 ProducerCnt 阈值 + stop 标志的优雅停机与本地逆元降级路径。代价是可维护性一般（大量魔数、手工内存布局、注释稀疏），且功能刻意保持最小（无 checkpoint、无分布式）。
