# 离线变速的时长契约（Offline Duration Contract）

本文基于 Rubber Band Library 4.0.0 的实际实现，说明**离线模式**（`OptionProcessOffline`，默认模式）下，预分析（study）、预告时长（expected/target duration）与尾部排空（draining）三者如何共同决定调用方最终能取到的音频区间，并对比 R2（Faster，`src/faster/`）与 R3（Finer，`src/finer/`）两个引擎各自承担的责任。

所有结论都标注了源码位置，行号对应当前工作区代码。

---

## 1. 总体数据流与三个时长概念

离线模式必须分两遍喂数据（见 `rubberband/RubberBandStretcher.h:70-83`）：

1. **预分析 pass**：对每块调用 `study(..., final)`，无输出；
2. **处理 pass**：对每块调用 `process(..., final)`，再循环 `available()` / `retrieve()` 取输出。

整个链路里存在三个互不等价的"时长"，它们由不同来源决定，也是本文讨论的核心：

| 概念 | 含义 | R2 中的载体 | R3 中的载体 |
|---|---|---|---|
| 预分析观测到的输入长度 | study pass 实际累计的帧数 | `m_inputDuration`（`R2Stretcher.h:227`） | `m_studyInputDuration`（`R3Stretcher.h:347`） |
| 调用方预告的输入长度 | `setExpectedInputDuration(n)` | `m_expectedInputDuration`（`R2Stretcher.h:170`） | `m_suppliedInputDuration`（`R3Stretcher.h:348`） |
| 目标/理论输出长度 | 内部据上面两者推算出的输出帧数 | `theoreticalOut`（局部变量，`StretcherProcess.cpp:1056-1059`） | `m_totalTargetDuration`（`R3Stretcher.h:349`） |

关键事实：**预告长度不会被校验为与实际输入一致，两个引擎在不一致时的处理方式完全不同**（见第 3 节）。

---

## 2. 两个引擎各自如何决定"可取音频区间"

### 2.1 前置填充与起点对齐（两者思路相同，实现不同）

离线模式下，引擎内部做了起点填充和延迟补偿，因此对外的
`getPreferredStartPad()` 与 `getStartDelay()` **恒返回 0**
（`R2Stretcher.cpp:835-860`、`R3Stretcher.cpp:511-543`，头文件
`RubberBandStretcher.h:683-684` 也明确承诺这一点）。填充与裁剪都在内部完成：

- **R2**：首次进入处理时，向每通道输入环预填 `m_aWindowSize/2` 个零
  （`R2Stretcher.cpp:1258-1266`）；写出时再丢弃开头
  `startSkip = lrintf((m_sWindowSize/2) / m_pitchScale)` 帧
  （`StretcherProcess.cpp:1139-1142`、`1174-1188`）。
- **R3**：首次处理时预填 `pad = getWindowSourceSize()/2` 个零
  （`R3Stretcher.cpp:761-769`），并记录
  `m_startSkip = round(pad / m_pitchScale)`（`R3Stretcher.cpp:773`），
  在每次写出后从输出环里跳过（`R3Stretcher.cpp:1146-1157`）。

也就是说，"可取区间的起点"由内部零填充 + 起始 skip 锁定为输入第 0 帧对应的位置，调用方无需、也不应再自行裁头。

### 2.2 R2（Faster）：预分析产出整条拉伸剖面，输出端只做"封顶"

**study 阶段真正分析音频。** `R2Stretcher::study()`
（`R2Stretcher.cpp:950-1094`）把混音数据写入分析环，按 `m_increment`
步进逐块做 FFT，累计瞬态检测曲线 `m_phaseResetDf` 与静音标志 `m_silence`，
并把实际输入帧数累加进 `m_inputDuration`（末尾扣除半窗填充，
`R2Stretcher.cpp:1085-1090`）。

**拉伸剖面在首次 process 时一次性算出。** `calculateStretch()`
（`R2Stretcher.cpp:1138-1176`）调用 `StretchCalculator::calculate()`
（`StretchCalculator.cpp:79-195`），生成每个分析块的输出步进序列
`m_outputIncrements`。这里有一个极易误判的实现细节：

```cpp
size_t outputDuration = lrint(inputDuration * ratio);   // 第 87 行：算了，但……
outputDuration = lrint((phaseResetDf.size() * m_increment) * ratio); // 第 91 行：被覆盖
```

传进来的 `inputDuration`（即预告长度，见下文第 3.1 节）算出的
`outputDuration` **立即被基于实际分析块数的值覆盖**，后续峰值映射
（`mapPeaks`）和步进分配全部以实际块数为准。因此 **R2 的拉伸剖面形状与总步进长度由 study 实际"看到"的音频决定，而不是由预告长度决定。**

**处理阶段按剖面消费块，尾部进入 draining。**

- 当 `process(..., final=true)` 时，每通道记录
  `cd.inputSize = cd.inCount`（`R2Stretcher.cpp:1315-1320`），
  这是唯一被"确认"的输入终点（字段注释见
  `StretcherChannelData.h:127`："set only after known (when data ended);
  -1 previously"）。
- `testInbufReadSpace()` 在已知终点且剩余不足半窗时置
  `cd.draining = true`（`StretcherProcess.cpp:385-415`）。
- draining 后不再分析新块，只把合成累加器里的残余按
  `accumulatorFill` 排干，最后一块把 `shiftIncrement` 收缩到剩余量并标记
  `last`（`StretcherProcess.cpp:470-486`），累加器清空时置
  `outputComplete = true`（`StretcherProcess.cpp:1118-1121`）。

**输出长度的硬封顶。** 每次 `writeChunk` 计算

```cpp
theoreticalOut = lrint(cd.inputSize * m_timeRatio);   // StretcherProcess.cpp:1056-1059
```

`writeOutput()` 一旦发现本批写出会越过 `theoreticalOut`，就把本批数量
`qty` 收缩到恰好补满目标值（`StretcherProcess.cpp:1148-1157`）。
注意它**只能截断、不能补齐**：若排干后天然产出不足 `theoreticalOut`，
不会补零延长。

**结束信号。** 非线程模式下，`available()` 在 `inputSize >= 0` 时还会
顺手驱动剩余块处理（`StretcherProcess.cpp:1210-1230`）；当所有通道
`outputComplete` 且输出环读空，返回 `-1`
（`StretcherProcess.cpp:1248-1251`）。

### 2.3 R3（Finer）：study 只数帧数，目标长度用于硬截断

**study 阶段不分析音频，只累加帧数。** `R3Stretcher::study()`
（`R3Stretcher.cpp:586-607`）的全部作用就是

```cpp
m_mode = ProcessMode::Studying;
m_studyInputDuration += samples;
```

没有 FFT、没有检测曲线。R3 的瞬态/相位引导是在处理过程中逐帧实时计算的
（`Guide::updateGuidance`，`R3Stretcher.cpp:1408-1438`）。

**目标输出长度在首次 process 时确定**（`R3Stretcher.cpp:725-739`）：

- 若做过 study：`m_totalTargetDuration = round(m_studyInputDuration * m_timeRatio)`；
- 若没做过 study（`JustCreated`）但调用过 `setExpectedInputDuration`：
  改用 `round(m_suppliedInputDuration * m_timeRatio)`；
- 两者都没有：`m_totalTargetDuration` 保持 0，即**不做任何目标长度约束**。

**处理与排干在 `consume()` 内同步完成。** 每帧的输出步进 `outhop` 由
`StretchCalculator::calculateSingle()` 即时给出
（`R3Stretcher.cpp:969-975`）。`final` 为真且输入环读空时，循环会继续到
合成累加器排空为止（`R3Stretcher.cpp:1018-1032`），
`synthesiseChannel` 在 draining 时逐帧削减 `accumulatorFill`
（`R3Stretcher.cpp:1656-1666`）。

**目标长度只用于截断。** 每次写出前（`R3Stretcher.cpp:1106-1115`）：

```cpp
if (m_totalTargetDuration > 0 &&
    m_totalOutputDuration + writeCount > m_totalTargetDuration) {
    writeCount = m_totalTargetDuration - m_totalOutputDuration;
}
```

与 R2 同样是"能截不能补"。`process(..., final=true)` 后立即把模式置为
`Finished`（`R3Stretcher.cpp:865-874`，注释明确说明不区分
"Finished" 与 "仍在排干"）；`available()` 是纯粹的输出环查询，
输出环读空且已 Finished 才返回 `-1`（`R3Stretcher.cpp:877-886`），
它本身**不会**像 R2 那样驱动额外计算——所有计算都已在 `process()` 内完成。

### 2.4 小结：可取区间 = [起始 skip 之后, 天然排干产出与目标长度的较小者]

| 环节 | R2 | R3 |
|---|---|---|
| study 是否分析音频 | 是，产出检测曲线与整条步进剖面 | 否，仅累计帧数 |
| 剖面/步进来源 | study 实际块数 → `m_outputIncrements` | 处理中逐帧 `calculateSingle` |
| 内部前填充 | `m_aWindowSize/2` 零 | `getWindowSourceSize()/2` 零 |
| 起始裁剪 | `writeOutput` 丢 `startSkip` | 输出环 `skip(m_startSkip)` |
| 目标输出长度 | `lrint(inputSize * timeRatio)`，逐写封顶 | `round(duration * timeRatio)`，逐写封顶 |
| 目标长度能否补长 | 否，只截断 | 否，只截断 |
| `available()` 是否驱动计算 | 是（非线程模式） | 否，纯查询 |
| 完成判据 | 各通道 `outputComplete` 且环空 → -1 | 环空且 `Finished` → -1 |

---

## 3. 长度不一致与排空不完整时，两个引擎的责任划分

### 3.1 预告长度 ≠ study 实际长度

**R2：以 study 实际长度为准，预告长度几乎只影响日志和窗口尺寸。**

`calculateStretch()` 在发现 `m_expectedInputDuration != m_inputDuration`
时只打一条警告，然后用预告值覆盖局部 `inputDuration`
（`R2Stretcher.cpp:1145-1150`）：

```
WARNING: Actual study() duration differs from duration set by
setExpectedInputDuration - using the latter for calculation
```

但如第 2.2 节所述，这个被覆盖的值传入 `StretchCalculator::calculate`
后，其算出的 `outputDuration` 立刻被实际块数派生值覆盖
（`StretchCalculator.cpp:87-91`）。预告长度在 R2 中真正生效的地方只有：

- `calculateSizes()` 里据此缩小输入步进，避免短素材被过大的步进跳过
  （`R2Stretcher.cpp:496-501`）；
- 上面那条警告日志。

**R3：study 与预告二选一，且 study 优先；两者不一致时没有任何警告。**

目标长度在首次 process 时按"做过 study 就用 `m_studyInputDuration`，
否则才用 `m_suppliedInputDuration`"选择（`R3Stretcher.cpp:727-739`）。
一旦做过 study，`setExpectedInputDuration` 设置的值在本次运行中**完全不参与
目标长度计算**，且引擎不会提示二者不符。

### 3.2 study 长度与 process 实际喂入长度不一致

这是更危险的情形，因为两个 pass 由调用方分别驱动，库不校验两遍输入的同一性。

**R2 的责任：** 拉伸剖面的块数是按 study 数据固定的。处理时
`getIncrements()` 按 `cd.chunkCount` 索引 `m_outputIncrements`，
块数超出剖面长度时把 `chunkCount` 钳到最后一个元素并返回
`gotData=false`（`StretcherProcess.cpp:685-695`），即**重复使用最后一个
步进**继续处理多出来的输入；输入不足时则提前进入 draining。
与此同时，输出端的 `theoreticalOut` 用的是 **process pass 的实际
`inputSize`** 而非 study 长度（`StretcherProcess.cpp:1057-1058`）。
于是会出现"剖面按 A 长度算、封顶按 B 长度算"的撕裂：库不报错，
后果（截断或末尾质量异常）由调用方承担。

**R3 的责任：** 没有固定剖面，逐帧计算天然适应任意实际输入长度，
不存在块数越界问题；但目标长度 `m_totalTargetDuration` 是按 study（或预告）
长度预算的固定值，处理逻辑只做单边截断：

- 实际输入更长 → 多余输入被处理，但输出在目标长度处被硬切
  （`R3Stretcher.cpp:1107-1114`），尾部音频丢失，无提示；
- 实际输入更短 → 排干后产出天然不足，输出短于目标长度，**不补零**。

### 3.3 排空（draining）不完整

"排空不完整"指 `final` 已给出，但调用方没有继续驱动到 `available()`
返回 `-1` 就停止取数。两个引擎都**没有后台保证**，责任在调用方：

- **R2**：`final=true)` 仅设置 `inputSize` 并把模式置 `Finished`
  （`R2Stretcher.cpp:1315-1320`、`1358`）。非线程模式下，剩余块要靠后续
  `available()` 调用在 `processChunks` 里懒驱动
  （`StretcherProcess.cpp:1221-1229`）；线程模式下靠工作线程
  （`StretcherProcess.cpp:61-65`）。若调用方不再调用
  `available()/retrieve()`，累加器与输出环里的尾部数据就永远滞留，
  库不会自行 flush，也无错误上报。
- **R3**：排干在 `process(..., final=true)` 内同步跑完
  （`consume` 的 final 循环，`R3Stretcher.cpp:1018-1032`），
  结果落入输出环；但调用方若不循环 `retrieve()` 到 `available() == -1`，
  环内数据同样不会被强制交出。`Finished` 状态本身不代表数据已被取走。

参考命令行工具的正确用法：process 循环结束后还有一个独立的
`while ((avail = ts.available()) >= 0)` 收尾循环
（`main/main.cpp:1008-1028`）。跳过这一步就会丢失尾部。

此外，若调用方**根本不给 `final=true`**：R2 的 `inputSize` 永远是 -1，
`testInbufReadSpace` 在数据不足一窗时直接返回 false、永不进入 draining
（`StretcherProcess.cpp:387-405`）；R3 的 `consume` 在非 final 下数据不足
即 break 等待（`R3Stretcher.cpp:1028-1031`），模式也不会进入
`Finished`。两种情况下 `available()` 都不会返回 -1，流看起来"永远没结束"。

---

## 4. 哪些状态可以确认，哪些时长只是推断

### 4.1 可以确认的状态（有确定的内部状态机/计数器支撑）

| 可确认项 | 依据 |
|---|---|
| 处理生命周期阶段 | R2 `m_mode`：`JustCreated/Studying/Processing/Finished`（`R2Stretcher.h:180-187`）；R3 同名枚举（`R3Stretcher.h:355-361`） |
| R2 输入终点已知 | `cd.inputSize` 从 -1 变为实际 `inCount`，仅发生在 final（`StretcherChannelData.h:127`、`R2Stretcher.cpp:1319`） |
| R2 是否处于排空 / 输出是否完成 | `cd.draining`、`cd.outputComplete` 两个显式布尔（`StretcherChannelData.h:130-131`） |
| R3 是否已 Finished | `m_mode == Finished`，但注释声明它不等价于"数据已取完"（`R3Stretcher.cpp:865-870`） |
| 此刻可取帧数 / 流结束 | `available()` 的非负值是输出环真实可读量；`-1` 是唯一的结束确认（R2 `StretcherProcess.cpp:1248-1251`；R3 `R3Stretcher.cpp:880-885`） |
| 离线起点对齐已内部处理 | 离线模式两个 pad/delay 查询恒为 0（`R2Stretcher.cpp:837,851`；`R3Stretcher.cpp:514,531`） |
| 实际累计的输入/输出计数（R3 内部） | `m_consumedInputDuration`、`m_totalOutputDuration` 逐帧累加（`R3Stretcher.cpp:1143-1144`）；R2 对应 `cd.inCount`、`cd.outCount` |

### 4.2 只是推断、不构成精确承诺的时长

| 推断量 | 为什么只是推断 |
|---|---|
| 目标输出长度 `theoreticalOut` / `m_totalTargetDuration` | 都是 `输入帧数 × timeRatio` 后 `lrint`/`round` 的**预算值**，仅用于写出端封顶；实际产出还取决于窗口累加、步进取整、重采样器帧数和排干结果，可能更短 |
| R2 的拉伸剖面总长度 | 由 `phaseResetDf.size() * m_increment * ratio` 取整得到（`StretchCalculator.cpp:91`），块级步进经 `round` 分配并带钳制（`StretchCalculator.cpp:484-491`），累计值只近似等于理想长度 |
| R3 每帧 `outhop` | `calculateSingle` 基于预期/实际输出帧的偏差动态修正并钳制在 `[0.3×, 2×]` 理想步进内（`StretchCalculator.cpp:469-502`），是自适应估计而非固定映射 |
| `setExpectedInputDuration` 描述的"精确输出" | 头文件措辞是"allows the stretcher to **ensure** that the number of output samples is exactly correct"（`RubberBandStretcher.h:776-780`），但实现上该值在 R2 几乎不决定剖面、在 R3 仅做单边截断，"exactly" 只在调用方同时保证 study/process 输入一致且排干完整时才成立 |
| 起始 skip 的帧数 | `round(pad / pitchScale)`（R3 `R3Stretcher.cpp:773`）、`lrintf((m_sWindowSize/2)/pitchScale)`（R2 `StretcherProcess.cpp:1141`）本身就是取整结果 |

### 4.3 当前实现没有提供的保证

1. **不校验两遍输入的同一性。** 库无法知道 study 与 process 收到的是不是同一段音频、长度是否相等；R2 仅在预告值与 study 值不符时打一条 log（`R2Stretcher.cpp:1146-1148`），R3 连 log 都没有。
2. **不保证输出一定达到目标长度。** 封顶逻辑只截不补（R2 `StretcherProcess.cpp:1153-1156`；R3 `R3Stretcher.cpp:1107-1114`）；实际输入偏短或排干产出不足时，输出会短于 `round(N × ratio)`。
3. **不保证尾部被自动交出。** 没有后台 flush；必须由调用方循环到 `available() == -1`（对比 `main/main.cpp:1009`）。
4. **不提供目标输出长度的查询 API。** `theoreticalOut` 是 R2 的局部变量，`m_totalTargetDuration` 是 R3 的 protected 成员；公开 API 只能通过 `available()` 看当前可读量，无法在处理前或处理中询问"最终应有多少帧"。
5. **不保证 study 可省略。** 离线模式跳过 study 时，R2 的 `m_outputIncrements` 为空，`getIncrements` 退化为恒返回默认 `m_increment`、`gotData=false`（`StretcherProcess.cpp:685-690`），即退化为无自适应的标称步进；R3 则回退到仅认 `setExpectedInputDuration`（`R3Stretcher.cpp:732-738`），两者都不是文档推荐路径。
6. **诊断信息不对称。** `getOutputIncrements()`、`getExactTimePoints()`、`calculateStretch()` 等剖面诊断仅 R2 支持，R3 一律返回空/无操作（`RubberBandStretcher.cpp:304-323,333-337`）；R3 的逐帧引导过程没有等价的公开观测手段。
7. **关键帧映射只到块精度。** `setKeyFrameMap` 的源帧在 R2 按 `m_increment` 量化到块（`StretchCalculator.cpp:238`），代码注释自陈"we … aren't [compensating] … yet"（`StretchCalculator.cpp:233-236`）；R3 在关键帧之间动态推算局部比率（`R3Stretcher.cpp:413-491`），映射点不提供采样级精确时刻保证，且关键帧本身不决定整体时长（头文件 `RubberBandStretcher.h:884-890`）。
8. **`available()==0` 的含义依赖上下文。** R2 线程模式下 0 可能只是"工作线程尚未产出"而非结束（头文件 `RubberBandStretcher.h:943-947`）；只有 `-1` 才是确定的终态。

---

## 5. 对调用方的实践要点

1. 离线模式务必让 **study 与 process 两遍输入逐帧一致、长度相等**，并在两遍都正确传递 `final`。
2. `setExpectedInputDuration(n)` 仍应调用且填真实帧数——R2 用它选择合适的窗口/步进（`R2Stretcher.cpp:496-501`），R3 在未 study 时用它建立目标长度；但不要把它当作输出长度的强制保证。
3. process 循环之后，必须再跑一个 `while (available() >= 0) retrieve(...)` 收尾循环，参照 `main/main.cpp:1008-1028`，否则尾部滞留。
4. 若需要精确的输出帧数，调用方应自己以 `round(N × timeRatio)` 预算并在写出端做最终对齐（截断或补零）；库内部只保证不超过该预算，不保证恰好达到。
5. 判断流是否真正结束只认 `available() == -1`，不要用 0 推断结束，尤其在 R2 多线程模式下。
