# 多声道伸缩的两种声道处理模式：源码分析报告

分析对象：Rubber Band Library（分支 `task/76`，提交基线 `e4296ac`）。
涉及的两个引擎：

- **R2（Faster，默认引擎）**：`src/faster/`（`R2Stretcher.cpp`、`StretcherProcess.cpp`、`StretcherChannelData.*`）
- **R3（Finer，`OptionEngineFiner`）**：`src/finer/`（`R3Stretcher.cpp`、`Guide.h`、`PhaseAdvance.h`、`R3LiveShifter.cpp`）

两个公开选项定义在 `rubberband/RubberBandStretcher.h:429-430`：

```cpp
OptionChannelsApart    = 0x00000000,   // 默认：独立声道
OptionChannelsTogether = 0x10000000,   // 联动声道
```

头文件对二者责任的定性（`RubberBandStretcher.h:366-386`）是：Apart 追求"单声道最大保真、牺牲同步"，结果是声像变宽、单声道兼容性下降；Together 追求"更高同步、牺牲一定的单声道保真"，目标是中央清晰度与单声道兼容性。本报告用实现证据核对这些承诺能走多远。

---

## 0. 两套机制先分清：M/S 变换 vs. 声道锁定

两种模式在两个引擎里的实现机制并不相同，这是后续所有结论的基础。

| 机制 | R2（Faster） | R3（Finer） |
|---|---|---|
| Together 是否做 M/S 编解码 | 是 | 是（仅严格 2 声道时） |
| M/S 编码位置 | `StretcherProcess.cpp:168-171, 206-211, 246-251`（`prepareChannelMS`，`StretcherProcess.cpp:129-147`） | `R3Stretcher.cpp:924-949`（`prepareInput`）；Live 版 `R3LiveShifter.cpp:391-408` |
| M/S 解码位置 | `StretcherProcess.cpp:1286-1296`（`retrieve`） | `R3Stretcher.cpp:907-916`；Live 版 `R3LiveShifter.cpp:663-672` |
| 频域跨声道相位耦合 | 无专门机制 | 有：`channelLock` 频段 + `GuidedPhaseAdvance`（`Guide.h:299-306`、`PhaseAdvance.h:160-235`） |
| 节奏/增量决策 | 全模式共享一份增量计划 | 全模式共享 `inhop/outhop`，但每声道各自做分类与引导 |

关键事实：**R2 的 Together 与 Apart 走的是同一套分块处理代码，唯一差别是前两级的 M/S 变换；R3 则在 M/S 之外还有一条真正的跨声道相位锁定通路，且该通路在 Apart 下也部分开启。**

---

## 1. 瞬态、相位选择、重采样、声像保持的逐项对照

### 1.1 瞬态（onset / transient）决策

#### R2：瞬态计划在两种模式下都由混合信号单一决定，天然同拍

- 离线 `study()` 把所有声道**等权求和成一条 mixdown** 再做检测（`R2Stretcher.cpp:973-994`），检测结果写入唯一的 `m_phaseResetDf`；`calculateStretch()` 据此产出**一份** `m_outputIncrements`（`R2Stretcher.cpp:1152-1173`）。
- 实时路径 `calculateIncrements()` 先断言各声道 `chunkCount` 相等（`StretcherProcess.cpp:548-553`，失配直接报 `"Channels are not in sync"` 并返回），再把各声道幅度谱**求和**喂给同一个检测器（`StretcherProcess.cpp:580-596`），算出的一份增量在 `processOneChunk()` 里被所有声道共用（`StretcherProcess.cpp:361-368`）。
- 每个声道通过 `getIncrements()` 读取的是**同一张** `m_outputIncrements` 表（`StretcherProcess.cpp:697-712`）。

结论：**R2 中瞬态发生在第几个分块、是否 phase reset、shift/phase increment 取多少，对两个声道永远是同一个答案——这与 Together/Apart 无关。** 头文件所说 R2 的 Apart"只在瞬态处同步"（`RubberBandStretcher.h:370-371`），其实现含义是：节奏网格始终共享，但非瞬态分块里各声道的相位被独立改写，只有 phase reset 点（瞬态/静音，`StretcherProcess.cpp:754-756` 处 reset 时"leaving phases unmodified"）让两条声道同时停止漂移、重新对齐到原始相位关系。

#### R3：节奏同拍，但瞬态分类与 kick/reset 是每声道独立产出的

- R3 没有离线预分析（`study()` 只累计时长，`R3Stretcher.cpp:587-607`）。每帧的 `inhop/outhop` 由**唯一**的 `StretchCalculator` 算出并对所有声道生效（`R3Stretcher.cpp:969-975`、`1038-1040` 的分析循环、`1126-1141` 的统一发射）。
- 但每个声道持有**独立的** `BinClassifier`、`BinSegmenter` 和 `Guidance`（`R3Stretcher.h:219-226`），`analyseChannel()` 逐声道调用 `m_guide.updateGuidance(...)`（`R3Stretcher.cpp:1408-1438`）。因此 `kick`、`preKick`、`phaseReset` 的频段在两条声道上可以不同。
- Together 的 M/S 路径有一处专门的不对称保护：side 声道（`c == 1`）**禁止因静音触发 phase reset**，理由写在注释里——reset 会传播到 mid，导致单声道素材被持续重置（`R3Stretcher.cpp:1400-1406`）。
- `PhaseAdvance` 里还有一条 M/S 专属规则：当 side 声道出现 phaseReset 时，mid 声道在同频段也按 reset 处理（`PhaseAdvance.h:201-203`）。

**同步边界承诺：**

- 两种模式都能承诺**样本级的节奏同步**（分块边界、hop 一致），因为决策数据结构是单份的。
- R3 Apart **不能**承诺瞬态处理动作（kick/reset 的频段）两声道一致；Together 通过 M/S 域处理 + 上述两条传播/抑制规则，把瞬态重置收敛到 mid 主导，换中央稳定性。
- R2 两种模式都能承诺瞬态**位置与重置时机**一致，但 Apart 不承诺重置点之间的相位关系。

### 1.2 相位选择

#### R2：每声道各自跑相位推进，Apart 与 Together 的差别只在输入域

- `modifyChunk()` 对每个声道独立做 laminar/independent 相位推进（`StretcherProcess.cpp:746-901`），状态（`prevPhase`、`unwrappedPhase`、`prevError`）全部存在各自的 `ChannelData` 里。
- Apart：两条独立声道各自推进，bin 间耦合只发生在**同一声道内部**（laminar 继承，`StretcherProcess.cpp:853-878`），声道之间无任何约束。
- Together：被推进的是 mid 和 side 两个"虚拟声道"，同样互不约束。

因此 R2 Together 的相位友好性完全来自 M/S 表示本身：见 1.4。

#### R3：存在显式的跨声道相位锁定，频段宽度由选项决定

`GuidedPhaseAdvance::advance()`（`PhaseAdvance.h:91-245`）的关键步骤：

1. 逐 bin 选出幅度最大的声道 `m_greatestChannel[i]`（`PhaseAdvance.h:160-171`）。
2. 对落在本声道 `channelLock` 频段内的 bin，若"最强声道"在同一 bin 也处于其 channelLock 频段，且两声道在该 bin 的峰值轨迹一致（`otherPrevPeak == prevPeak`），则**借用最强声道的峰值推进量**计算本声道输出相位（`PhaseAdvance.h:212-231`）。

而锁定频段的宽度在 `Guide.h:299-306` 决定：

```cpp
guidance.channelLock.present = true;
guidance.channelLock.f0 = 0.0;
if (tighterChannelLock) {          // OptionChannelsTogether
    guidance.channelLock.f1 = nyquist;   // 全频带锁定
} else {
    guidance.channelLock.f1 = 600.0;     // Apart：只锁 600 Hz 以下
}
```

并且在极端拉伸比（ratio > 2.0）下，锁定上限会被逐步压低（每超 1.0 降 150 Hz，地板 100 Hz，`Guide.h:421-430`）——**即便是 Together，"全频带锁定"在大拉伸比下也不成立**。另外静音（`updateForSilence`）和 unity 路径（`updateForUnity`）提前 return，`channelLock.present` 保持 false（`Guide.h:222, 286-297, 480-522`），此时相位由 reset/unlocked 规则接管。

**保真/同步边界承诺：**

- R3 Together 可承诺：在非静音、非 unity、拉伸比适中的帧里，**0 Hz–Nyquist 的峰值相位推进跨声道一致**（条件是峰值轨迹匹配）；R3 Apart 只对 **600 Hz 以下**做此承诺，高频允许各声道独立发散——这正对应头文件"低频同步维持得比高频紧"的描述（`RubberBandStretcher.h:368-370`）。
- 两种模式都不承诺 bin 级相位逐点相等：锁定走的是"峰值 + β 加权的相位差保留"（`ph = peakNew + beta * diff`，`PhaseAdvance.h:227-231`），保留的是声道间相对相位结构，而非把两声道相位写成同一个值。
- R2 的任何模式都**没有**频域跨声道锁定机制，不承诺频域相位同步；Apart 下连 M/S 域的结构性保护都没有。

### 1.3 重采样

#### 实例拓扑不同，这是最容易被忽略的实现差异

- **R2：每声道一个独立 `Resampler` 实例**，构造参数通道数为 1（`R2Stretcher.cpp:673` 与 `810`：`new Resampler(params, 1)`），保存在 `ChannelData::resampler`（`StretcherChannelData.h:136`）。Together 的 M/S 模式下，mid、side 各用一个独立实例（`StretcherProcess.cpp:206-218` 的预重采样、`writeChunk` 的后重采样 `StretcherProcess.cpp:1087-1092`）。
- **R3：全引擎一个多通道 `Resampler`**，按总声道数构造（`R3Stretcher.cpp:307-308`：`new Resampler(params, m_parameters.channels)`），一次 `resample()` 调用处理全部声道（`R3Stretcher.cpp:821-827`、`1091-1097`；Live 版 `R3LiveShifter.cpp:415-421`）。

默认的 BQ 重采样器内部把多声道交织进**单一相位状态机**：`current_channel` 在声道间循环，只有绕回 0 时滤波器相位才推进一步（`BQResampler.cpp:583-629`）。所以 R3 的多声道重采样在结构上保证每条声道**消费/产出帧数完全一致、滤波相位同拍**；R2 的两个独立实例则各自维护滤波器历史与相位，输出帧数靠相同的输入帧数和相同 ratio 隐式趋同，`retrieve()` 再用"取各声道最小值"兜底（见第 2 节）。

#### 时序位置也不同

- R2：根据 pitch 选项决定重采样发生在伸缩之前或之后（`resampleBeforeStretching()`，`StretcherProcess.cpp:119-127`）。
- R3：离线一律后重采样；实时按 HighSpeed/HighQuality/Consistency 分流（`areWeResampling()`，`R3Stretcher.h:431-484`）。

**边界承诺：**

- R3 可承诺重采样环节**不引入声道间帧数差或相位差**（单状态机、同相位）。
- R2 只能承诺**期望帧数**一致（同 ratio、同 incount），独立滤波器状态意味着瞬态响应的数值细节两声道不必相同；Together 下被重采样的是 M/S 信号，帧数差会在解码后同时污染 L 与 R（见 1.4），不会只影响一侧。
- 重采样本身是线性时变操作，两种模式都不改变"共享样本数≠声像正确"这一事实——它只保证长度，不保证 L/R 比例。

### 1.4 声像（stereo image）保持

#### Together 的核心资产：M/S 域处理

编码（两引擎相同形式）：`m=(l+r)/2, s=(l-r)/2`（`StretcherProcess.cpp:136-146`、`R3Stretcher.cpp:934-941`）；解码：`l=m+s, r=m-s`（`StretcherProcess.cpp:1288-1295`、`R3Stretcher.cpp:908-915`）。

- 中央声像（l≈r）几乎全部能量落在 mid，side 近似为零。对 mid/side 分别施加**相同的非线性伸缩处理**，等价于让中央素材主要承受"单声道式"的形变；解码后两声道共享同一份形变，中央不会漂移。
- 单声道兼容性：单声道折叠即 `(l+r)/2 = m`，side 在折叠中抵消。只要 side 的处理不产生与 mid 相干的伪影，折叠结果就由 mid 单独决定——这正是头文件"preserve mono compatibility"（`RubberBandStretcher.h:381-382`）的实现依据。R3 禁止 side 静音 reset（`R3Stretcher.cpp:1400-1406`）就是在专门堵住 side 伪影污染 mid 的路径。
- 代价：side 承受与 mid 同等强度的形变，宽度/空间感被压缩，即头文件说的"relatively less stereo space and width"（`RubberBandStretcher.h:383-384`）；R2 下 M/S 两通道还各自独立做相位推进，mid 与 side 之间可能产生处理引入的相位差，解码后体现为声像模糊。

#### Apart 的行为

- R2 Apart：L、R 完全独立处理。非瞬态区间各自的相位发散在折叠求和时变成梳状/相位抵消——头文件明确警告"mono mixes … can sound phasy"（`RubberBandStretcher.h:373-375`），声像会异常变宽。
- R3 Apart：600 Hz 以下仍有 channelLock（`Guide.h:305`），低频（bass、基音、脚鼓这些决定单声道兼容听感的成分）仍被跨声道约束，高频自由。因此 R3 Apart 的 mono 兼容边界**优于** R2 Apart，但高频声像仍可能展宽。

**声像承诺清单：**

| 承诺 | R2 Apart | R2 Together | R3 Apart | R3 Together |
|---|---|---|---|---|
| 输出帧数两声道相等 | 是（兜底裁剪） | 是 | 是（架构保证） | 是 |
| 瞬态/节奏点同拍 | 是 | 是 | 是 | 是 |
| 频域相位跨声道约束 | 无 | 无（仅 M/S 结构） | ≤600 Hz | 全频带（受 ratio>2 衰减条款限制） |
| 中央声像稳定 | 不承诺 | 结构性支持 | 低频支持 | 最强 |
| 单声道折叠无相位伪影 | 不承诺 | 显著改善但非数学保证 | 低频改善 | 最接近承诺，仍非数学保证 |
| 立体声宽度保留 | 最好（可能异常变宽） | 压缩 | 较好 | 压缩 |

注意 M/S 编解码是线性无损的（半精度系数在解码时精确还原），**但中间的伸缩处理对 mid、side 是两个独立的非线性信道**，所以整条链路不是透明的：没有任何代码校验解码后的 L/R 比例与输入一致。

---

## 2. 兼容边界：模式切换、双声道缺失、单声道兼容

### 2.1 模式切换：构造期固定，生命周期内不可变

- 头文件明确"These options may not be changed after construction"（`RubberBandStretcher.h:362-364`）。代码中不存在任何 `setChannelsOption`；R3 的 `setPitchOption` 对任何构造后改动直接打印"not supported in R3 engine"（`R3Stretcher.cpp:261-264`），选项位保存在 `const` 语义的 `m_parameters.options` 中。
- `reset()` 只清缓冲与状态、**不动 options**（`R3Stretcher.cpp:552-584`；R2 同理重置 `ChannelData` 但保留 `m_options`）。因此 reset 不能用于切换模式。
- 实际切换方式只有销毁重建实例。重建意味着所有滤波器历史、相位历史、ring buffer、R2 的 study 结果全部作废，流内无缝切换在 API 层面不被支持——调用方必须自行做交叉淡化。
- 另一条隐式边界：选项是位掩码，库会**静默忽略不认识的位**。头文件因此建议构造后调用 `getEngineVersion()` 确认实际引擎（`RubberBandStretcher.h:177-184`）：向旧版库传 `OptionChannelsTogether` 与 `OptionEngineFiner` 的组合，可能静默落到 R2，语义与 R3 并不相同（见 1.2 的机制差异）。

### 2.2 双声道缺失（声道数 ≠ 2 时选项的实际效力）

两个引擎对"不是两声道"的处理不对称，且都是静默降级，不报错：

**R3 / R3LiveShifter：门控条件最严格——必须恰好 2 声道。**

```cpp
bool useMidSide() const {
    return m_parameters.channels == 2 &&
        (m_parameters.options & OptionChannelsTogether);
}
```
（`R3Stretcher.h:486-490`、`R3LiveShifter.h:380-384`）

- 单声道：M/S 关闭，`PhaseAdvance` 中 `channels > 1` 分支不执行（`PhaseAdvance.h:160-174`），`tighterChannelLock` 虽仍传入但没有"另一个声道"可锁。选项实质空转。
- 3 声道及以上：M/S 同样关闭，所有声道按独立声道处理；channelLock 机制仍在多声道间生效（`m_greatestChannel` 支持任意声道数），但 Together 与 Apart 的差别只剩锁定频段 600 Hz vs Nyquist 这一条。**没有任何环绕声/多声道配对概念。**

**R2：门控是 `>= 2` 且只作用于前两个声道，存在多声道下的不对称。**

- 编码侧条件：`ChannelsTogether && m_channels >= 2 && c < 2`（`StretcherProcess.cpp:168-171`）——只有 0、1 号声道被换成 M/S，第 2 声道及以后保持原始信号进入各自缓冲。
- 解码侧条件只检查 `m_channels >= 2`（`StretcherProcess.cpp:1286-1287`），无条件把输出 0、1 号按 M/S 反变换。
- 由此得到一条明确的兼容边界：**R2 + 3 声道以上 + Together 时，前两个声道是 M/S 耦合对，其余声道完全独立，整条总线处于两种责任模式混用的状态**；库不阻止这种配置，也没有警告。
- 单声道下 R2 的 M/S 分支整体不触发（`c < 2` 但 mixdown 路径仍按单声道正常工作），选项同样空转。

### 2.3 声道失衡与"双声道缺失"的运行时表现

- 两个引擎的 `retrieve()` 都按**各声道可读样本数的最小值**裁剪返回长度，并在 c>0 出现短缺时打印 `"channel imbalance detected"`（R2：`StretcherProcess.cpp:1276-1284`；R3：`R3Stretcher.cpp:897-905`）。R2 的 `available()` 取所有声道 outbuf 的最小值（`StretcherProcess.cpp:1239-1246`）；R3 的 `available()` 只看 0 号声道（`R3Stretcher.cpp:877-886`），失衡完全依赖 retrieve 时的兜底。
- 这是一种**长度对齐**保护，不是声像保护：被裁剪掉的样本直接丢弃，没有补偿、没有淡化。它保证返回缓冲形状整齐，不保证两声道内容对称。
- 实际触发概率：R3 因为单 hop 决策 + 单重采样器，架构上不产生帧数差，该警告属于防御性代码；R2 的独立 per-channel 重采样器（1.3）理论上具备产生帧数差的状态条件，兜底路径有实际意义。

### 2.4 单声道兼容性（mono fold-down）

- 输入本身是单声道素材、却以 2 声道实例处理（L=R）：Together 下 side≈0，处理主要作用于 mid，折叠稳定；R3 还专门关闭 side 的静音 reset 以防单声道素材引发反复重置（`R3Stretcher.cpp:1400-1406`）。这是 Together 对单声道素材最友好的一条显式证据。
- Apart 下 L=R 也会被当作两条独立材料处理，R2 无跨声道约束，折叠可能出现相位抵消；R3 靠 600 Hz 以下锁定缓解。
- 边界：库**不提供**单声道折叠输出接口，也不检测输入是否为双单声道（dual-mono）；所有 mono 兼容结论都是处理结构的推论，而非被验证的输出性质。

---

## 3. 结论：以实现证据为依据的承诺边界

1. **"共同样本数"是节奏同步的产物，不是声像正确性的充分条件。** 两引擎都用单份 hop/增量计划（R2：`m_outputIncrements` 单表 + mixdown 检测，`R2Stretcher.cpp:973-994, 1152-1173`；R3：单 `StretchCalculator` 与统一 emit 循环，`R3Stretcher.cpp:969-975, 1126-1141`），所以两声道输出等长是结构性必然。但等长只说明第 n 个输出样本在时间上对齐；该样本的 L/R 能量比、相位差是否保持输入声像，取决于相位推进（R3 `PhaseAdvance.h:212-231`）、M/S 域非线性处理（1.4）和独立重采样状态（R2），没有任何一处对声像做校验或约束性保证。`retrieve()` 的最小裁剪（2.3）只对齐长度，丢弃样本且不补偿。

2. **联动模式（Together）能承诺的，是"同步优先"的一组结构性条件，而非透明传输：**
   - R2：M/S 表示把中央素材与侧边素材分离处理，给单声道折叠和中央稳定提供结构性保护；但无频域跨声道锁定，mid/side 仍是两个独立非线性信道，宽度被压缩，相位伪影只能缓解不能消除。
   - R3：在 M/S 之上额外提供全频带 channelLock（`Guide.h:302-303`）与 side reset 抑制/传播规则（`R3Stretcher.cpp:1400-1406`、`PhaseAdvance.h:201-203`），是四者中同步承诺最强的；但锁定在 ratio>2 时按频段衰减（`Guide.h:421-430`）、静音/unity 帧失效，且锁定保留的是相对相位结构而非相位相等。

3. **独立模式（Apart）能承诺的，是"单声道保真优先"：** 每声道独立的分类、相位推进与（R2 的）独立重采样状态，避免一个声道的处理决策拖累另一个；代价是 R2 完全不承诺跨声道相位关系与 mono 兼容，R3 仅承诺 600 Hz 以下的低频锁定。

4. **兼容边界是硬编码的静默降级，调用方必须自己防御：** 模式构造后不可切换、reset 不改模式（2.1）；R3 仅在恰好 2 声道时启用 M/S，R2 在 >2 声道时只耦合前两对且不警告（2.2）；失衡只裁长度不保声像（2.3）；库不提供、也不验证 mono 折叠正确性（2.4）。

**给调用方的可操作结论：** 需要 mono 兼容或中央稳定（广播、采样素材、再折叠场景）选 Together，且优先 R3；追求宽度与单声道保真、且下游始终保持立体声的选 Apart；任何情况下都不应把 `available()`/等长输出作为声像正确的判据，声像验证必须测量输出 L/R 的电平差与相位差（或直接测量 fold-down 后的频谱损失），而不是核对样本计数。

---

## 附：关键证据索引

| 主题 | 文件:行 |
|---|---|
| 选项位定义 | `rubberband/RubberBandStretcher.h:429-430` |
| 模式责任的官方定性 | `rubberband/RubberBandStretcher.h:362-386` |
| R2 M/S 编码 | `src/faster/StretcherProcess.cpp:129-147, 168-171` |
| R2 M/S 解码 | `src/faster/StretcherProcess.cpp:1286-1296` |
| R2 共享瞬态计划（离线 mixdown） | `src/faster/R2Stretcher.cpp:973-994, 1152-1173` |
| R2 实时共享增量与同步断言 | `src/faster/StretcherProcess.cpp:548-553, 580-596, 361-368` |
| R2 每声道独立相位推进 | `src/faster/StretcherProcess.cpp:746-901` |
| R2 每声道独立重采样器 | `src/faster/R2Stretcher.cpp:673, 810`；`src/faster/StretcherChannelData.h:136` |
| R3 M/S 门控（恰好 2 声道） | `src/finer/R3Stretcher.h:486-490`；`src/finer/R3LiveShifter.h:380-384` |
| R3 M/S 编解码 | `src/finer/R3Stretcher.cpp:907-916, 924-949` |
| R3 channelLock 频段决策 | `src/finer/Guide.h:299-306, 421-430` |
| R3 跨声道相位借用 | `src/finer/PhaseAdvance.h:160-174, 201-203, 212-231` |
| R3 side 静音 reset 抑制 | `src/finer/R3Stretcher.cpp:1394-1406` |
| R3 单实例多通道重采样器 | `src/finer/R3Stretcher.cpp:282-308, 821-827, 1091-1097` |
| BQ 重采样单相位状态机 | `src/common/BQResampler.cpp:583-629` |
| 声道失衡兜底（两引擎） | `src/faster/StretcherProcess.cpp:1276-1284`；`src/finer/R3Stretcher.cpp:897-905` |
| R3 构造后拒绝选项变更 | `src/finer/R3Stretcher.cpp:261-264` |
| R3 reset 不触碰 options | `src/finer/R3Stretcher.cpp:552-584` |
