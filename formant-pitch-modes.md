# Rubber Band 中"共振峰保持"与"音高改变"的责任边界

> 依据代码：Rubber Band Library v4.0.0（`meson.build` 中 `version: '4.0.0'`）。
> 本文只陈述可以从本仓库源码直接确认的事实；凡属听感、频谱质量评价的部分，均显式标注为"代码无法确认"，不引用、也不虚构任何未实际执行的听测结果。
> 引用格式为 `文件:行号`，均相对仓库根目录。

## 0. 结论速览

- 音高改变在两个引擎里都**不是靠在频谱里搬动 bin 实现的**，而是"相位声码器做时间伸缩 + 采样率重采样把时长拉回去"的经典组合。重采样器是音高改变的实际承担者，频谱/相位处理承担的是时间伸缩与相干合成。
- "共振峰保持"是一条**只改幅度谱、不碰相位**的旁路处理，通过倒谱（cepstrum）低通分离频谱包络，再对包络做反向重映射。它与相位处理是两条独立的路径。
- `OptionFormantPreserved` 这个名字只表示"启用了上述包络校正算法"，**不等于**"输出音色被保证保持"。算法有明确的作用范围和近似手段，保真程度代码本身无法证明。

---

## 1. 频谱包络/相位处理 与 采样比例 各自承担什么

### 1.1 音高改变 = 内部时间伸缩 × 重采样

两个引擎对音高移位的总体思路一致，R2 的注释把原理写得最直白：

```
// A frequency shift is achieved using an additional time shift,
// followed by resampling back to the original time shift to
// change the pitch.
```
（`src/faster/R2Stretcher.cpp:370-375`）

- 内部时间伸缩器使用的有效比率是 `m_timeRatio * m_pitchScale`：
  - R2：`src/faster/R2Stretcher.cpp:377`（`R2Stretcher::getEffectiveRatio`）
  - R3：`src/finer/R3Stretcher.h:422-424`（`R3Stretcher::getEffectiveRatio`）
- 重采样器以 `1.0 / m_pitchScale` 的比率再做一次采样比例换算，把时长恢复到 `m_timeRatio` 规定的目标时长，频谱内容则整体被搬移 `m_pitchScale` 倍：
  - R2 后置重采样：`src/faster/StretcherProcess.cpp:1087-1092`；前置重采样：`src/faster/StretcherProcess.cpp:213-218`
  - R3 后置重采样：`src/finer/R3Stretcher.cpp:1091-1097`；前置重采样：`src/finer/R3Stretcher.cpp:821-827`
- 重采样器质量参数固定取 `Resampler::FastestTolerable`（`src/finer/R3Stretcher.cpp:287`、`src/faster/R2Stretcher.cpp:658`），具体后端可以是 IPP / libsamplerate / Speex / 内置 BQResampler（`src/common/Resampler.cpp:173,600,825,992`）。

因此，采样比例（resample ratio）承担的责任是：**实际改变信号的频率位置（音高），同时抵消内部时间伸缩引入的时长变化**。它是一个时域波形级操作，不区分谐波与包络——谐波和共振峰包络会被它一起搬移。

### 1.2 频谱与相位处理承担的责任

**R2（Faster 引擎，`src/faster/`）**：单 FFT 尺寸的传统相位声码器。

- 分析：加窗、FFT、转极坐标得到 `mag`/`phase`（`src/faster/StretcherProcess.cpp:725-743`，`analyseChunk`）。
- 修改：`modifyChunk` 逐 bin 做相位偏差校正（phase unwrap / laminar 相位锁定，`src/faster/StretcherProcess.cpp:746` 起，`laminar` 判定在 `:763`），服务于**时间伸缩后的相干重合成**，不做 bin 频率搬移。
- 合成：逆 FFT、窗口累加、归一化（`synthesiseChunk`，`src/faster/StretcherProcess.cpp:962` 起）。

**R3（Finer 引擎，`src/finer/`）**：多分辨率（多个 FFT 尺寸并行、按频段混合）的引导式相位声码器。

- 每个 FFT 尺寸各自分析出幅度/相位（`R3Stretcher::analyseChannel`，`src/finer/R3Stretcher.cpp:1286-1338`）。
- 相位由 `GuidedPhaseAdvance` 依据自适应频率引导（guide）、相位锁定带（phase-lock bands）、峰值拾取等推进（`src/finer/PhaseAdvance.h:39-98`，调用点 `src/finer/R3Stretcher.cpp:1042-1065`）。
- 各频段分别逆 FFT 后在时域累加混合（`synthesiseChannel`，`src/finer/R3Stretcher.cpp:1562-1668`）。

也就是说，频谱/相位路径的责任是：**在不重采样的前提下改变时间轴（time ratio），并保证合成帧之间相位连续、抑制瞬态模糊与相位混乱**。它本身不实现音高乘数。

### 1.3 重采样放在频谱处理之前还是之后

这是一个可由代码确认、但随模式变化的分工点。

R2 的规则（`src/faster/StretcherProcess.cpp:113-127`，`resampleBeforeStretching()`）：

| 条件 | 重采样位置 |
|---|---|
| 离线模式 | 一律后置（代码注释说明离线的 stretch 计算依赖后置顺序，`:115-118`） |
| 实时 + `OptionPitchHighQuality` | pitchScale < 1 时前置，否则后置（注释标注 "better sound"） |
| 实时 + `OptionPitchHighConsistency` | 一律后置 |
| 实时 + `OptionPitchHighSpeed`（默认） | pitchScale > 1 时前置（注释标注 "better performance"），否则后置 |

R3 的规则（`src/finer/R3Stretcher.h:431-484`，`areWeResampling()`）：

| 条件 | 重采样位置 |
|---|---|
| 离线模式 | 一律后置（`:443-458`，注释给出三条历史原因） |
| 实时 + HighConsistency | 一律后置 |
| 实时 + HighQuality | 上移（scale>1）后置，下移（scale<1）前置 |
| 实时 + HighSpeed | 上移前置，下移后置 |

注意：`OptionPitch*` 选项只影响实时模式下重采样器的摆位与动态比率行为；R3 中 `setPitchOption` 在构造后直接被拒绝（`src/finer/R3Stretcher.cpp:261-264`），R2 离线模式下该选项不生效（`src/faster/R2Stretcher.cpp:932-935`）。

### 1.4 共振峰包络处理是第三条、独立的路径

共振峰处理只在幅度谱上发生，两处实现都不修改相位数组：

- R2：`R2Stretcher::formantShiftChunk`（`src/faster/StretcherProcess.cpp:905-960`），在 `synthesiseChunk` 内、逆 FFT 之前调用（`:968-971`）。
- R3：`R3Stretcher::analyseFormant` / `adjustFormant`（`src/finer/R3Stretcher.cpp:1454-1524`），在相位推进之前调用（`:1340-1343`）；实时变体 `R3LiveShifter` 中算法相同（`src/finer/R3LiveShifter.cpp:983-1053`，调用点 `:869-872`）。

它与另外两条路径的分工可以概括为：

| 路径 | 操作对象 | 承担的责任 |
|---|---|---|
| 重采样器（采样比例） | 时域波形 | 实际音高乘数；谐波与包络被整体搬移 |
| 相位声码器（频谱 + 相位） | 相位谱为主、幅度参与窗口归一 | 时间伸缩与相位相干合成 |
| 共振峰校正（倒谱包络） | 仅幅度谱 | 试图把被重采样一起搬走的频谱包络搬回原频率位置 |

---

## 2. 两套处理模式如何分工

"两套模式"在本库中存在两个维度，分别说明，避免混淆。

### 2.1 维度一：两个引擎（R2/Faster 与 R3/Finer）

引擎在构造时固定，`OptionEngineFaster`（默认，值为 0）建 R2，`OptionEngineFiner`（`0x20000000`）建 R3（`src/RubberBandStretcher.cpp:79-89`；枚举见 `rubberband/RubberBandStretcher.h:432-433`）。

两者在共振峰问题上的**算法同源、参数不同**：

| 对比项 | R2（`StretcherProcess.cpp:905-960`） | R3（`R3Stretcher.cpp:1454-1524`） |
|---|---|---|
| 包络提取 | 幅度谱 → `inverseCepstral`（先取 log 再做逆 FFT，见 `src/common/FFT.cpp:384-392` 等）→ 倒谱低通 → 正 FFT → exp | 同原理 |
| 倒谱截止频率 | `sampleRate / 700`（`:921`） | `sampleRate / 650.0`（`:1469`） |
| 包络表示 | exp 一次（对数幅度域，`:935`） | exp 后再平方（功率域，`:1481-1482`），并封顶 `1.0e10`（`:1484-1486`） |
| 作用的 FFT 尺寸 | 单一 `m_fftSize` | 遍历全部多分辨率尺度（`:1496`），包络本身在 segmenter 的 FFT 尺寸上计算，再线性插值重采样到各尺寸（`FormantData::envelopeAt`，`R3Stretcher.h:201-211`） |
| 校正方式 | 先从 `mag` 除掉原包络，再按 pitchScale 重映射包络索引后乘回（`:936-957`），上移/下移分别用两个方向的循环避免覆盖（`:938-955`） | 对每个目标 bin 求 `source/target` 包络比值并乘到 `mag`（`:1513-1520`），比值钳制在 `[1/60, 60]`（`:1506-1518`） |
| 频率上限 | 全频带（`hs+1`） | 只处理到 10 kHz 对应 bin（`highBin`，`:1501,1512`） |
| 独立 formant scale | 不支持，`getFormantScale()` 恒返回 0.0（`src/RubberBandStretcher.cpp:155-157`） | 支持 `setFormantScale`（v3.0 起，头文件声明 `rubberband/RubberBandStretcher.h:632-635`） |
| 触发条件 | 选项为 preserved **且** `m_pitchScale != 1.0`（`:968-969`） | 仅判断选项位（`:1340`），pitchScale=1 时内部比率自然退化（见 2.3） |

### 2.2 维度二：两个共振峰选项（Shifted 与 Preserved）

枚举定义（`rubberband/RubberBandStretcher.h:422-423`）：

- `OptionFormantShifted = 0x00000000`（默认）：不做任何包络校正。重采样把谐波和频谱包络整体搬移，即头文件所说"The spectral envelope will be pitch shifted as normal"（`:328-330`）。
- `OptionFormantPreserved = 0x01000000`：启用 1.4 节的倒谱包络校正。两个引擎都受影响（头文件 `:325`），且可在任意时刻通过 `setFormantOption` 切换（`rubberband/RubberBandStretcher.h:757-764`；R2 实现 `R2Stretcher.cpp:920-927`，R3 实现 `R3Stretcher.cpp:251-258`，都是位掩码替换）。

两者的分工是**有无之分，不是两种不同算法之分**：preserved 只是在 shifted 的数据流中插入一段幅度谱校正，相位路径、重采样路径完全不变。

### 2.3 R3 的 `setFormantScale` 与自动语义

可由代码确认的语义（`src/finer/R3Stretcher.cpp:1490-1524`）：

- `m_formantScale` 默认 0.0（构造函数 `:42`）；为 0.0 时实际使用 `1.0 / m_pitchScale`（`:1503-1504`）。这正好抵消重采样对包络的搬移，数学上对应"包络留在原频率"。
- 显式设为 1.0 时 `sourceFactor == targetFactor`，比值恒为 1，幅度不被修改——与 shifted 的数据流结果等价（但分析步骤仍会执行）。
- 非零时按 `sourceFactor = targetFactor / formantScale` 做包络重映射，可制造独立于音高的共振峰移位特效。
- 离线模式下，处于 studying/processing 状态时调用会被拒绝并记录日志（`:239-245`）。

**一处文档与实现的偏差，需要特别指出**：公共头文件声称"如果 setFormantScale 设为非 0 值，无论 Preserved/Shifted 选项状态如何都会发生 formant shifting"（`rubberband/RubberBandStretcher.h:621-623`，`RubberBandLiveShifter.h:219-223` 措辞相同）。但本版本代码中，`analyseFormant/adjustFormant` 的唯一调用点被选项位独占守卫（`R3Stretcher.cpp:1340`、`R3LiveShifter.cpp:869`），没有任何 `m_formantScale != 0.0` 的旁路判断。即按当前源码，仅设置非零 formant scale 而不打开 `OptionFormantPreserved`，校正函数不会被调用。头文件描述的语义在本版本代码中无法复现，这是文档与实现不一致，而不是可以替任何一方辩护的"预期行为"。

---

## 3. 哪些结论可以由实现直接确认，哪些仍需额外证据

### 3.1 可直接由代码确认的参数/行为语义

以下结论有明确的枚举值、分支条件或算式支撑，可视为实现契约：

1. 选项位数值与默认值：Shifted=0（默认）、Preserved=`0x01000000`（`rubberband/RubberBandStretcher.h:422-423`）。
2. 触发条件：R2 要求 preserved 且 pitchScale≠1.0（`StretcherProcess.cpp:968-969`）；R3 仅看选项位（`R3Stretcher.cpp:1340`）。
3. 包络提取方法是倒谱低通（log 幅度 → 逆 FFT → 截止 → 正 FFT → exp），截止 quefrency 分别对应 700 Hz（R2）与 650 Hz（R3）。
4. R3 校正只作用到 10 kHz（`R3Stretcher.cpp:1501`），包络比值钳制在 60 倍以内（`:1506-1518`），包络插值为线性（`R3Stretcher.h:201-211`），包络估计值封顶 1e10（`:1484-1486`）。
5. 共振峰校正只写 `mag`，不写 `phase`/`advancedPhase`（两引擎的 adjust 函数体内均无相位赋值）。
6. 自动 formant scale 的算式是 `1/pitchScale`（R3 `:1504`）；R2 的包络重映射直接用 `m_pitchScale` 做索引（`StretcherProcess.cpp:941,952`），等价语义。
7. 音高改变的实际频率乘数由重采样比率 `1/pitchScale` 决定（四个重采样调用点），频谱相位代码只消费 `timeRatio*pitchScale` 的有效时间比率。
8. `setFormantScale` 仅 R3 家族有效，R2 恒返回 0.0（`src/RubberBandStretcher.cpp:155-157`）。
9. 重采样质量档位固定为 `FastestTolerable`，后端按编译条件选择（`Resampler.cpp`）。
10. 选项可运行时切换（位掩码替换），但 R3 离线处理中拒绝改 formant scale、R3 任何时候拒绝构造后改 pitch option。
11. 2.3 节指出的头文件与实现不一致，本身也是可确认的事实。

### 3.2 代码无法确认、需要额外证据的听感/频谱质量结论

以下说法在仓库中**没有**测试或测量支撑（`src/test/` 下无任何 formant 专项测试，grep 无命中），不能从实现推出，如需主张必须另行设计实验：

1. **"Preserved 保持了音色/说话人特征"**。代码只能证明包络被按倒谱低通估计做了反向重映射；倒谱截止（650/700 Hz）是粗糙的源-滤波器分离，谐波细密结构与包络的划分本身有近似，估计误差有多大、听感上是否"像同一个人"，需要客观指标（如包络谱偏差、共振峰频率 F1/F2 提取偏差）与主观听测共同验证。
2. **R3 相比 R2 的共振峰处理"质量更高"**。头文件对 R3 整体有 "higher-quality results ... especially ... vocals" 的定性表述（`rubberband/RubberBandStretcher.h:169-175`），但仓库内没有对应的对比测量；两者截止频率、功率/幅度域、插值、限幅策略均不同，具体差异方向需要频谱测量与盲听实验。
3. **10 kHz 上限、60 倍钳制、1e10 封顶这些经验常数的听感后果**。它们是可确认的工程约束，但在极端移位比率下是否产生可闻的高频包络断裂或泵动，代码给不出答案。
4. **重采样前置与后置的音质差异**。源码注释只给了 "better sound" / "better performance" 的简短标注（`StretcherProcess.cpp:120-126`、`R3Stretcher.h:443-482`），没有数据；两种摆位改变的是 FFT 分析到的素材长度与瞬态位置，实际失真需测量。
5. **`FastestTolerable` 重采样档位在音高移位链路中的可闻代价**。档位选择是事实，但它与相位声码器失真叠加后的总体听感无法从代码推断。
6. **跨帧包络估计的时间连续性**。R3 每帧独立估计包络（无跨帧平滑代码），R2 同样逐 chunk 计算；是否产生包络抖动/颤音感，需要时序频谱分析或听测。
7. **头文件中 "without so substantially affecting the perceived pitch profile"（`:332-335`）这类感知措辞**，属于文档作者的经验陈述，仓库内无实验记录，引用时应注明其来源是文档而非验证数据。

### 3.3 本文刻意不做的事

- 不把 `OptionFormantPreserved` 这个选项名称当作"音色保持"的保证：它保证的是**某段校正代码被执行**，不是感知结果。
- 不报告任何 A/B 听测结论、MOS 分数或频谱图对比——这些工作在本仓库内不存在，本文未执行任何音频渲染或测量，虚构此类结果属于无据断言。
- 不把头文件的宣传性措辞与实现事实混为一谈；两者冲突时（见 2.3），以代码路径为准并显式标出冲突。

---

## 附：关键代码索引

| 主题 | 位置 |
|---|---|
| 选项枚举与文档 | `rubberband/RubberBandStretcher.h:323-335, 422-423, 609-637, 757-765` |
| 引擎选择 | `src/RubberBandStretcher.cpp:79-89` |
| R2 有效比率（时间×音高） | `src/faster/R2Stretcher.cpp:363-378` |
| R2 重采样摆位规则 | `src/faster/StretcherProcess.cpp:112-127` |
| R2 共振峰校正 | `src/faster/StretcherProcess.cpp:904-960`，调用点 `:968-971` |
| R2 前后置重采样 | `src/faster/StretcherProcess.cpp:213-218`、`:1087-1092` |
| R3 有效比率与摆位规则 | `src/finer/R3Stretcher.h:422-484` |
| R3 共振峰分析/校正 | `src/finer/R3Stretcher.cpp:1453-1524`，调用点 `:1340-1343` |
| R3 formant scale 设值/限制 | `src/finer/R3Stretcher.cpp:236-258` |
| R3 相位推进 | `src/finer/PhaseAdvance.h:39-239`；`src/finer/R3Stretcher.cpp:1042-1065` |
| R3 重采样调用 | `src/finer/R3Stretcher.cpp:821-827`、`:1091-1097` |
| 倒谱变换原语 | `src/common/FFT.cpp:384-418` 等 |
| 重采样器质量与后端 | `src/finer/R3Stretcher.cpp:286-308`；`src/common/Resampler.cpp:173,600,825,992` |
| 实时变体（同构实现） | `src/finer/R3LiveShifter.cpp:869-872, 983-1053` |
