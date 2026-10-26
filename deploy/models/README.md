# Qwen3.8-27B / 单张 RTX 3080

[返回 shuming](../../README.md) · [配置参考](../../docs/reference.md)

**本次仅提供默认配置、进程间接口和启动参数草案。没有下载权重、启动GPU服务、实测显存/速度或验收工具调用。** 3080的10/12GB具体版本及主机内存尚待确认，以下不是“已经部署成功”。

## 部署边界

```text
本地开发机：
  llama-server (GPU + CPU卸载) :8010 → OpenAI兼容 /v1/chat/completions
        ↑
  shuming C++ :9006 → BGE / RAG → 独立测试MySQL
        ↑
  EMOS / Next.js :3000
```

此处的`remote_engine`是**进程外API**，模型可以与后端同机；不是要求把后端部署到数据库服务器。既有CI/Staging和数据卷不变。

- 生成模型：[Qwen官方模型卡](https://huggingface.co/Qwen/Qwen3.8-27B)。
- 量化格式：Q4_K_M，参考[llama.cpp维护者的GGUF仓库](https://huggingface.co/ggml-org/Qwen3.8-27B-GGUF)。这是量化发布方，不冒称Qwen官方GGUF。
- 27B × 4 bit理论下限约13.5GB，**还没包括元数据、KV、计算缓冲**；10/12GB显存不应按全GPU容纳来配置。必须允许CPU/主机内存承载其余权重；32GB主机内存也只可视为准备起点，不是容量或性能保证。
- 默认8层GPU卸载、单并发、8192上下文是保守的接口初值，不是实测最优。内存不足时先缩上下文/批量，减少GPU层；吞吐和首字延迟以后再测。
- 默认仅文本：`supports_images=false`，不启用视觉投影、视频或OCR。模型原生支持多模态，不代表当前服务部署已接好视觉接口。
- BGE嵌入仍为原512维模型；不要换成27B生成模型或重建已有索引。

## 1. 独立安装模型服务

本项目内嵌llama.cpp固定了兼容修订，用于原有后端/BGE路径；**本次不升级它，也不承诺该修订支持Qwen3.8**。另行准备支持该模型的llama-server。下面记录了核对文档时的上游修订，仅用于复现起点，尚未在3080编译/运行：

```bash
git clone https://github.com/ggml-org/llama.cpp ../llama.cpp-qwen38
git -C ../llama.cpp-qwen38 checkout b3fa69955ea81c3b60e77e3c1e2224c4a826ffb9
cmake -S ../llama.cpp-qwen38 -B ../llama.cpp-qwen38/build \
  -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build ../llama.cpp-qwen38/build --target llama-server --parallel 2
```

需事先准备合适驱动/CUDA工具链；不要因此修改生产机驱动。RTX 3080的计算能力为8.6，见[NVIDIA硬件表](https://developer.nvidia.com/cuda/gpus)。构建说明见[上游文档](https://github.com/ggml-org/llama.cpp/blob/b3fa69955ea81c3b60e77e3c1e2224c4a826ffb9/docs/build.md)。

从GGUF仓库的Files选择实际Q4_K_M文件，记录所用仓库修订和文件SHA-256；本次没有替你下载或核验权重，不编造校验值。放置为`models/Qwen3.8-27B-Q4_K_M.gguf`，或用`MODEL_PATH`指定实际绝对路径。分片权重需完整保留同目录，指向首片；不要只改名首片而破坏分片命名。文件不进入Git。

## 2. 查看计划，再显式启动

需要Node.js >=22。仓库根目录执行：

```bash
# 只输出参数，不读权重、不启动进程、不下载
node deploy/models/local-model.mjs plan

# 以下由操作者在模型主机执行，本次没有运行
LLAMA_SERVER_BIN=/absolute/path/to/llama-server \
MODEL_PATH=/absolute/path/to/model-Q4_K_M.gguf \
node deploy/models/local-model.mjs run
```

`run`默认选择GPU0；可通过`CUDA_VISIBLE_DEVICES`选择实际单卡。`MODEL_GPU_LAYERS`和`MODEL_THREADS`覆盖默认8层/4线程；`MODEL_GPU_LAYERS=0`退到CPU。主进程前台运行，Ctrl-C停止。不会配置systemd、Docker或防火墙。

端口固定绑定`127.0.0.1:8010`，不启用公网监听或鉴权。
`local-no-auth`只是现有客户端需要非空Key时使用的**非秘密占位字符串**，不是访问控制。不要把它用于真实云服务。存在不可信本机用户或需要跨主机访问时，应另做鉴权、TLS和网络隔离，而不是公开这个未鉴权端口。

启动参数启用Jinja并关闭thinking，降低小显存阶段的额外输出；工具调用序列仍需后续真实模型验证。参数来源见[上游server说明](https://github.com/ggml-org/llama.cpp/blob/b3fa69955ea81c3b60e77e3c1e2224c4a826ffb9/tools/server/README.md)和Qwen模型卡。

## 3. 后端与PDF接口配置

`config.example.json`的新安装默认值与`qwen38-27b-3080.json`对齐：

```json
{
  "remote_engine": {
    "endpoint": "http://127.0.0.1:8010/v1",
    "model": "Qwen3.8-27B",
    "api_key": "local-no-auth",
    "context_window_tokens": 8192,
    "supports_images": false
  }
}
```

API别名与启动参数`--alias Qwen3.8-27B`一致。后端继续发送标准聊天/SSE/tool请求，未新增聊天或MCP路由。

已有配置**不自动覆盖**。可以从原配置创建一个仅切换模型相关字段的新文件：

```bash
mkdir -p .tmp
node deploy/models/local-model.mjs config config.json .tmp/qwen38-3080-config.json
# 后续由操作者选择 --config .tmp/qwen38-3080-config.json 启动后端
```

保留MySQL/JWT/租户等原配置；生成端点、别名、上下文及占位Key切为本地配置，关闭query rewrite，收紧工具结果字节预算为6000，Agent超时放宽到300秒。新文件权限0600，已存在则拒绝覆盖，原文件不变；不会打印配置内容或凭据。`.tmp/`已忽略。

后端和PDF处理统一读取`REMOTE_ENGINE_ENDPOINT`、`REMOTE_ENGINE_MODEL`、`REMOTE_ENGINE_API_KEY`；非空通用环境变量优先于JSON，旧`DEEPSEEK_API_KEY`仅在无显式Key时兼容回退。入口endpoint是`/v1`，不要填完整`/chat/completions`路径给C++端。

PDF脚本的长文截取、输出预算和排队需要以后按8192实际上下文调整；本次不宣称长篇论文处理已兼容验收。query rewrite默认关闭，避免每次检索额外调用27B；确认速度后可再启用。

## 容器与已有服务

这个配置的网络假设是**模型与调用端在宿主机原生进程运行**。Docker里的127.0.0.1是容器本身，不能直接套用。已有本地/服务器Compose未因本次改动迁移；需要同网络的模型服务别名、受控代理或其他明确的网络方案，并同时给后端/PDF进程配置可达endpoint。不要为“连上模型”盲目绑定0.0.0.0。

旧HTTPS云模型配置仍可显式使用，旧服务器的`configure-model.py`仍用于其HTTPS配置，不用于这个本地HTTP流程。网页搜索/抓取仍是独立可选DeepSeek服务；Qwen生成服务不会自动提供搜索API，需分别配置授权，默认关闭。BGE、MySQL、生产数据卷和GitHub workflow均未改动。

## 验证状态与硬件验收

仅检查JSON、启动计划、参数边界、新配置不覆盖/权限及文档路径，不调用GPU或模型。没有进行完整C++编译、Python运行测试、实际HTTP/SSE、工具链、图片或PDF长文、并发、显存与性能测试；这些均为待验收项。此配置尚未完成 RTX 3080 硬件验收，不能视为可直接上线的部署保证。

### 验收记录要求

实际部署后，在独立验证记录中保存以下信息，不以项目首页介绍替代测试结论：

- GPU 型号与显存容量、主机内存、驱动/CUDA 和 llama-server 修订。
- 权重来源、量化类型、文件校验值与实际启动参数。
- 模型加载、首字延迟、生成吞吐、显存与主机内存峰值。
- HTTP/SSE、工具调用、长篇 PDF 和目标并发下的成功/失败情况。

不同机器、权重和上下文长度的结果应分别记录，避免把配置初值写成普适性能结论。
