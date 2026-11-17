# 关键帧时间映射契约（Keyframe Map Contract）

本文档基于对仓库中 `setKeyFrameMap()` 实现路径的分析，说明 Rubber Band Library
关键帧映射的实际语义。核心结论：**传入映射只是声明意图，库只在其内部量化粒度
与全局时间比的约束下"尽力"满足锚点**——接受映射不等于满足实际时序。

涉及代码：

- 公共 API：`rubberband/RubberBandStretcher.h:868-892`（`setKeyFrameMap`）
- C API：`src/rubberband-c.cpp:152-159`（`rubberband_set_key_frame_map`）
- R2（Faster）引擎：`src/faster/R2Stretcher.cpp:325-340` +
  `src/common/StretchCalculator.cpp`（`setKeyFrameMap` / `mapPeaks` / `calculate`）
- R3（Finer）引擎：`src/finer/R3Stretcher.cpp:267-279`（设置）、
  `412-491`（`updateRatioFromMap`）、`741-748`（每个 process 轮次调用）

---

## 1. 时间锚点（Time Anchors）

### 1.1 映射方向与单位

映射为 **源素材采样帧号 → 拉伸输出采样帧号**（均为 sample frame，多声道按帧计，
不是按单个采样计）。只应提供关键帧，且关键帧之间应有"合理"间隔
（`RubberBandStretcher.h:873-874`）。

### 1.2 隐含的 0 → 0 锚点

R2 路径中，若映射非空但缺少源帧 0 的条目，库会自动插入 `0 → 0`
（`StretchCalculator.cpp:69-76`）。**R3 路径不做此插入**：它直接用映射的
第一个条目推导初始时间比 `first->second / first->first`
（`R3Stretcher.cpp:417-424`），等价于假设从原点到首个关键帧是线性段。
因此两个引擎对"首个关键帧不在 0"的映射解释不同，跨引擎复用同一映射时应注意。

### 1.3 锚点精度是量化后的，不是采样级精确的

- **R2**：源帧号按分析块（chunk，大小为 `m_increment`，即 hop）整除量化
  （`StretchCalculator.cpp:238`）。代码注释明确承认：目标采样应当按 chunk 位置
  与请求源采样之间的偏差做补偿，"但我们还没有做"（`StretchCalculator.cpp:232-236`）。
  即锚点只保证到**最近的一个 chunk**。
- **R3**：比值切换发生在 `process()` 调用边界——每轮处理检查已消耗输入时长
  是否越过下一个关键帧（`R3Stretcher.cpp:437, 746-748`），精度受处理块大小与
  hop 量化双重限制。

### 1.4 映射不决定总时长

关键帧映射只约束素材**内部**的点；整体拉伸比（输出时长 / 输入时长）必须另行
通过 `setTimeRatio()` 提供（`RubberBandStretcher.h:884-890`）。两个引擎的最终
区域都延伸到 `outputDuration = 总输入 × timeRatio`：

- R2：`StretchCalculator.cpp:91` 直接由 ratio 计算输出时长，最后一个关键帧之后
  的区域按比例铺满到该时长；
- R3：越过最后一个关键帧后，以 `(studyInputDuration, totalTargetDuration)`
  作为隐含的终点锚（`R3Stretcher.cpp:450-451`）。

若 `timeRatio` 与映射末端隐含的比值不一致，输出会被**截断或意外拉长**，
且库不报错——这是"接受映射 ≠ 满足时序"最直接的体现。

---

## 2. 重复与逆序映射（Duplicate / Reversed Mappings）

### 2.1 重复源帧

C++ 接口类型为 `std::map<size_t, size_t>`，同一源帧只能有一个目标；C 接口
逐条 `kfm[from[i]] = to[i]`（`rubberband-c.cpp:156`），**后写覆盖先写，不报警**。
源帧顺序无需调用者保证——map 按键自动排序，因此"源逆序"在类型层面无法表达。

### 2.2 目标逆序（输出时间倒流）

目标值逆序（后一个关键帧的输出帧号 ≤ 前一个）是可以构造的，两个引擎均
**不拒绝，但各自降级**：

- **R2**：`mapPeaks` 跳过任何 `targetStartSample >= targetEndSample`
  （或源起 ≥ 源终、超出总时长）的区段，仅打日志
  `"NOTE: ignoring key-frame mapping ..."`（`StretchCalculator.cpp:250-257`）。
  该锚点被静默丢弃，区段由相邻锚点线性插值覆盖。
- **R3**：若下一关键帧的输出帧号不超过当前已越过的锚点输出，打日志
  `"previous target key frame overruns next key frame"`，并把输出增量钳为 1
  （`R3Stretcher.cpp:467-472`），即该段以近乎冻结的极小比值推进，**不会倒放**。
  源帧号不递增（相对已消耗位置）时，比值回退为 1.0（`R3Stretcher.cpp:479-482`）。

### 2.3 越界锚点

- R2：源 chunk ≥ 总 chunk 数或目标 ≥ 总输出时长的锚点被忽略（同上 250-257 行）；
  区域边界在 `calculate()` 中被钳到 `[0, totalCount]` / `[0, outputDuration]`
  （`StretchCalculator.cpp:128-134`）。
- R3：超出输入总时长的锚点永远不会被越过，自然不生效。

**契约要点：非法映射不会失败，只会被忽略或降级，且只在 debug 日志中可见。
调用方若需要强保证，必须自行校验映射的单调性与边界。**

---

## 3. 实时模式（Real-Time Mode）

`setKeyFrameMap()` 在实时模式下**不可用**：两个引擎都检查 `m_realtime`，
打日志 `"Cannot specify key frame map in RT mode"` 后直接返回
（`R2Stretcher.cpp:328-331`，`R3Stretcher.cpp:269-272`）。公共头文件同样声明
"This function cannot be used in RealTime mode"（`RubberBandStretcher.h:876`）。

实时模式的等价手段是**随时调用 `setTimeRatio()`**（不得与 `process()` 并发，
库内无互斥锁，需调用方自行同步，见 `RubberBandStretcher.h:111-129`）。
即：离线模式用"预规划的分段映射"，实时模式用"流式改比值"，二者互斥，
由构造时的 `OptionProcessOffline / OptionProcessRealTime` 固定，不可切换。

---

## 4. 处理开始后的限制（Restrictions After Processing Starts）

离线模式下的时序约束（`RubberBandStretcher.h:878-882` 及各引擎实现）：

1. **必须在首次 `process()` 之前调用**。引擎检查处理状态，已开始则拒绝：
   `R2Stretcher.cpp:332-335`、`R3Stretcher.cpp:273-276`
   （"Cannot specify key frame map after process() has begun"）。
2. **应在设置时间与音高比之后调用**；调用后再改比值，结果**未定义**
   （头文件原文 "the results ... are undefined"）。离线模式下比值本就在
   `study()` 开始后冻结，因此实际可用窗口是：构造/`reset()` → 设比值 →
   设映射 → `study()`/`process()`。
3. **`reset()` 会清空映射**（`StretchCalculator.cpp:526`、`R3Stretcher.cpp:565`），
   但保留当前时间/音高比（`reset()` 文档，`RubberBandStretcher.h:543-548`）。
   复用实例处理下一段素材时必须重新设置映射。
4. 所有"拒绝"路径都只是记录日志并忽略，**不抛异常、不返回错误码**；
   C API 同样无错误反馈。调用方无法从返回值判断映射是否生效。

---

## 5. 瞬态保护的取舍（Transient Protection Tradeoffs）

关键帧映射与瞬态检测共用同一套"固定点"机制，但优先级与语义不同：

### 5.1 R2：映射锚点是"软"固定点

`StretchCalculator` 在 study 阶段从起始检测函数提取峰：软峰（一般 onset，
只校正时间位置）与硬峰（宽带瞬态，触发相位重置以保护瞬态形状，
见 `findPeaks` 注释 `StretchCalculator.cpp:536-546`）。引入映射后：

- 映射产生的锚点 **`hard = false`——"映射只在时间上，不是相位重置点"**
  （`StretchCalculator.cpp:262-264`）。把关键帧压在瞬态上**不会**触发瞬态保护；
  瞬态形状的保护仍完全取决于检测器与 `OptionTransientsCrisp/Mixed/Smooth` 选项。
- 仅当检测到的峰恰好落在关键帧所在 chunk 时，该锚点才升级为硬峰
  （`StretchCalculator.cpp:280-284`）。
- 关键帧之间的检测峰按区段内线性比例重新插值（`StretchCalculator.cpp:293-300`），
  即瞬态的**输出位置**服从映射分段，而非全局比值。
- 若插值结果使相邻固定点过近（目标间距 ≤ 一个 increment），检测峰被丢弃
  （`StretchCalculator.cpp:302-306`）：**密集的关键帧会挤掉瞬态锚点**，
  时序精度的获得以牺牲瞬态对齐为代价。

### 5.2 R3：映射只调制比值

R3 中映射不参与峰/相位逻辑，`updateRatioFromMap()` 只是逐段改写
`m_timeRatio` 并重算 hop（`R3Stretcher.cpp:486-487`）。瞬态处理完全由
R3 自身的分析/预读机制负责，与映射正交。取舍点在于：比值切换发生在
process 块边界，段边界附近的瞬态可能落在切换点的任一侧，锚点时刻同样
只有块级精度。

### 5.3 共同取舍

- **时间精度 vs 瞬态保真**：硬峰（相位重置）保护瞬态形状但带来
  "phasier" 的音色（`OptionTransientsMixed` 文档，`RubberBandStretcher.h:205-213`）；
  `OptionTransientsSmooth` 不重置相位，瞬态会涂抹但更平滑。映射锚点一律走
  软路径，等价于为映射选择了"不重置相位"这一侧。
- **量化 vs 精确**：两个引擎都优先保证内部调度的整数性（chunk/hop 对齐、
  区段输出凑整到 `regionDuration`，`StretchCalculator.cpp:148-185`），
  采样级精确的锚点被有意放弃——注释中明确标记为尚未实现的改进
  （`StretchCalculator.cpp:232-236`）。

---

## 6. 契约小结

| 主题 | 契约 |
|---|---|
| 锚点语义 | 源帧→输出帧；R2 自动补 0→0，R3 用首条目推初始比值 |
| 锚点精度 | R2 量化到 chunk；R3 量化到 process 块边界；均非采样级 |
| 总时长 | 始终由 `setTimeRatio()` 决定，映射末端不决定终点 |
| 重复源帧 | 后者覆盖前者，无告警 |
| 逆序/越界 | 不报错：R2 忽略该区段，R3 降级为近冻结或 ratio=1.0 |
| 实时模式 | 不可用（静默忽略）；改用动态 `setTimeRatio()` |
| 调用时序 | 设比值之后、首次 `process()` 之前；`reset()` 后需重设 |
| 错误反馈 | 无返回值/异常，仅 debug 日志 |
| 瞬态保护 | 映射锚点为软固定点，不触发相位重置；密集映射会抑制瞬态锚点 |
