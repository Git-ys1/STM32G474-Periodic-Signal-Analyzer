# PC分析工具

| 文件 | 作用 |
|---|---|
| `waveform_lab_core.py` | ADC生成、相位折叠、Huber与谐波投影公共库 |
| `custom_waveform_lab.py` | 自定义周期信号实验室GUI |
| `run_custom_waveform_lab.bat` | Windows一键启动GUI |
| `generate_g_problem_adc_tests.py` | 生成题内ADC测试数组 |
| `analyze_huber_phase_fold.py` | Huber干净/污染对照 |
| `analyze_phase_coverage.py` | 有理共振和相位覆盖扫描 |
| `analyze_256k_frequency_sensitivity.py` | 256 kHz频率敏感性分析 |
| `analyze_real_adc_smoothing.py` | 三组真实ADC的V2.2算法对照 |
| `requirements-waveform-lab.txt` | Python依赖 |

运行结果统一写入`tests/`。`__pycache__/`不进入仓库。
