"use client";

import { useState, useCallback, useRef, useEffect } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";

interface Message {
  id: string;
  role: "user" | "assistant";
  content: string;
  reasoning: string;
  isStreaming: boolean;
}

interface Session {
  id: string;
  title: string;
  model: string;
  created_at: string;
  updated_at: string;
}

const API_BASE = process.env.NEXT_PUBLIC_API_BASE || "";
const DEMO_PROMPT = "介绍一下林镇涛师兄关于MOE的流量的负载均衡相关的研究";
const PAPER_DOWNLOAD_URL = "/papers/FlowDyn-Dynamic-Flowlet-Gap-Detection.pdf";
const DEMO_ANSWER = `林镇涛师兄这篇论文可以从“动态调节 flowlet 时间”来理解。论文关注的是数据中心多路径网络中的负载均衡问题：传统 ECMP 容易因为哈希碰撞导致部分路径拥塞，而按包负载均衡虽然利用率高，却容易造成包乱序。flowlet 负载均衡折中地把一条长流拆成多个由时间间隔分开的短流段，再把不同 flowlet 调度到不同路径上。

核心问题在于 flowlet gap，也就是判断新 flowlet 的时间阈值应该如何设置。固定阈值很难适配实时网络状态：阈值太小，不同路径的延迟差还没有被吸收，后续包换路后可能先到达，造成乱序；阈值太大，又会错过很多可以重新选路的机会，负载均衡效果变差。

FlowDyn 的主要思想是把 flowlet gap 从静态参数变成动态参数。系统通过主动探测和遥测信息估计 ToR 交换机之间不同路径的时延，再根据路径时延差动态计算合适的 flowlet gap。这样可以在避免明显包乱序的前提下，更积极地把后续 flowlet 分散到拥塞更小的路径上。

放到 MoE 场景中，这个思路很有价值。MoE 的 token 路由会让不同专家、不同机器之间的通信流量持续变化，某些专家在短时间内可能成为热点，导致链路拥塞和尾延迟升高。动态 flowlet 时间可以根据网络状态调整流量切分粒度，让突发通信更均匀地走多条路径，从而缓解专家通信热点，提高多机多卡训练或推理的吞吐稳定性。

简单概括，这篇工作的贡献不是只讨论模型层面的专家均衡，而是从网络数据面的角度处理 MoE 通信负载：通过动态调节 flowlet 时间，在减少乱序风险的同时提升多路径链路利用率。`;

const demoSessions: Session[] = [
  {
    id: "demo-session",
    title: "MoE 流量负载均衡研究",
    model: "qwen3b-q4",
    created_at: "2026-06-24",
    updated_at: "2026-06-24",
  },
];

function PaperDownloadCard() {
  return (
    <div className="mt-4 rounded-lg border border-slate-200 bg-slate-50 p-3">
      <div className="flex items-center gap-3">
        <div className="flex h-10 w-10 shrink-0 items-center justify-center rounded-md bg-red-50 text-xs font-semibold text-red-700">
          PDF
        </div>
        <div className="min-w-0 flex-1">
          <div className="truncate text-sm font-medium text-slate-900">
            FlowDyn: Dynamic Flowlet Gap Detection.pdf
          </div>
          <div className="mt-1 text-xs text-slate-500">
            EMOS 文档库 · 论文附件 · 本地服务器
          </div>
        </div>
        <a
          className="shrink-0 rounded-md bg-slate-950 px-3 py-2 text-xs font-medium text-white hover:bg-slate-800"
          href={PAPER_DOWNLOAD_URL}
          download
        >
          下载论文
        </a>
      </div>
    </div>
  );
}

export default function Home() {
  const [messages, setMessages] = useState<Message[]>([]);
  const [input, setInput] = useState(DEMO_PROMPT);
  const [loading, setLoading] = useState(false);
  const [token, setToken] = useState("");
  const [loggedIn, setLoggedIn] = useState(false);
  const [demoMode, setDemoMode] = useState(false);
  const [username, setUsername] = useState("admin");
  const [password, setPassword] = useState("");
  const [loginError, setLoginError] = useState("");
  const [showReasoning, setShowReasoning] = useState<Record<string, boolean>>({});
  const [sessions, setSessions] = useState<Session[]>([]);
  const [activeSession, setActiveSession] = useState<string>("");
  const [sidebarOpen, setSidebarOpen] = useState(true);
  const messagesEndRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [messages]);

  const startDemo = useCallback(() => {
    setToken("demo-token");
    setLoggedIn(true);
    setDemoMode(true);
    setLoginError("");
    setActiveSession("demo-session");
    setSessions(demoSessions);
    setMessages([]);
    setInput(DEMO_PROMPT);
  }, []);

  const fetchSessions = useCallback(async () => {
    if (!token || demoMode) return;
    try {
      const res = await fetch(`${API_BASE}/api/sessions`, {
        headers: { Authorization: `Bearer ${token}` },
      });
      const data = await res.json();
      setSessions(data.sessions || []);
    } catch {}
  }, [token, demoMode]);

  useEffect(() => {
    if (!loggedIn || demoMode || !token) return;

    let cancelled = false;
    const loadSessions = async () => {
      try {
        const res = await fetch(`${API_BASE}/api/sessions`, {
          headers: { Authorization: `Bearer ${token}` },
        });
        const data = await res.json();
        if (!cancelled) setSessions(data.sessions || []);
      } catch {}
    };

    void loadSessions();
    return () => {
      cancelled = true;
    };
  }, [loggedIn, demoMode, token]);

  const createSession = useCallback(async () => {
    if (demoMode) {
      setActiveSession("demo-session");
      setSessions(demoSessions);
      setMessages([]);
      setInput(DEMO_PROMPT);
      return;
    }

    try {
      const res = await fetch(`${API_BASE}/api/sessions`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${token}`,
        },
        body: JSON.stringify({ title: "New Chat" }),
      });
      const data = await res.json();
      if (data.id) {
        setActiveSession(data.id);
        setMessages([]);
        fetchSessions();
      }
    } catch {}
  }, [token, fetchSessions, demoMode]);

  const switchSession = useCallback(async (id: string) => {
    setActiveSession(id);
    setMessages([]);
    if (demoMode) {
      setInput(DEMO_PROMPT);
      return;
    }

    try {
      const res = await fetch(`${API_BASE}/api/sessions/${id}`, {
        headers: { Authorization: `Bearer ${token}` },
      });
      const data = await res.json();
      if (data.messages) {
        const loaded: Message[] = data.messages.map(
          (m: { id: number; role: string; content: string; reasoning: string }) => ({
            id: String(m.id),
            role: m.role as "user" | "assistant",
            content: m.content || "",
            reasoning: m.reasoning || "",
            isStreaming: false,
          })
        );
        setMessages(loaded);
      }
    } catch {}
  }, [token, demoMode]);

  const deleteSession = useCallback(
    async (id: string, e: React.MouseEvent) => {
      e.stopPropagation();
      if (demoMode) {
        setMessages([]);
        setActiveSession("");
        return;
      }

      try {
        await fetch(`${API_BASE}/api/sessions/${id}`, {
          method: "DELETE",
          headers: { Authorization: `Bearer ${token}` },
        });
        if (activeSession === id) {
          setActiveSession("");
          setMessages([]);
        }
        fetchSessions();
      } catch {}
    },
    [token, activeSession, fetchSessions, demoMode]
  );

  const login = useCallback(async () => {
    try {
      const res = await fetch(`${API_BASE}/api/auth/login`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username, password }),
      });
      const data = await res.json();
      if (data.token) {
        setToken(data.token);
        setLoggedIn(true);
        setDemoMode(false);
        setLoginError("");
      } else {
        setLoginError(data.error || "Login failed");
      }
    } catch {
      setLoginError("后端未连接，可以先使用演示登录。");
    }
  }, [username, password]);

  const streamDemoAnswer = useCallback((msgId: string) => {
    const chunks = DEMO_ANSWER.split(/(\n\n|。|：|；)/).filter(Boolean);
    let index = 0;
    const timer = window.setInterval(() => {
      const chunk = chunks[index];
      if (!chunk) {
        window.clearInterval(timer);
        setMessages((prev) => {
          const updated = [...prev];
          const idx = updated.findIndex((m) => m.id === msgId);
          if (idx >= 0) updated[idx].isStreaming = false;
          return updated;
        });
        setLoading(false);
        return;
      }

      setMessages((prev) => {
        const updated = [...prev];
        const idx = updated.findIndex((m) => m.id === msgId);
        if (idx >= 0) updated[idx].content += chunk;
        return updated;
      });
      index++;
    }, 90);
  }, []);

  const sendMessage = useCallback(async () => {
    if (!input.trim() || loading) return;

    let sid = activeSession;
    if (!sid) {
      if (demoMode) {
        sid = "demo-session";
        setActiveSession(sid);
        setSessions(demoSessions);
      } else {
        try {
          const res = await fetch(`${API_BASE}/api/sessions`, {
            method: "POST",
            headers: {
              "Content-Type": "application/json",
              Authorization: `Bearer ${token}`,
            },
            body: JSON.stringify({ title: input.slice(0, 40) }),
          });
          const data = await res.json();
          if (data.id) {
            sid = data.id;
            setActiveSession(sid);
            fetchSessions();
          }
        } catch {
          return;
        }
      }
    }

    const userMsg: Message = {
      id: Date.now().toString(),
      role: "user",
      content: input,
      reasoning: "",
      isStreaming: false,
    };
    setMessages((prev) => [...prev, userMsg]);

    const msgId = (Date.now() + 1).toString();
    const assistantMsg: Message = {
      id: msgId,
      role: "assistant",
      content: "",
      reasoning: demoMode ? "命中 EMOS 文档库：FlowDyn 论文、MoE 通信流量、动态 flowlet 负载均衡。" : "",
      isStreaming: true,
    };
    setMessages((prev) => [...prev, assistantMsg]);
    setInput("");
    setLoading(true);

    if (demoMode) {
      streamDemoAnswer(msgId);
      return;
    }

    try {
      const res = await fetch(`${API_BASE}/api/chat/stream`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${token}`,
          "X-Request-Id": crypto.randomUUID(),
        },
        body: JSON.stringify({ message: userMsg.content, session_id: sid }),
      });

      const reader = res.body?.getReader();
      if (!reader) throw new Error("No reader");

      const decoder = new TextDecoder();
      let buffer = "";

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });

        const lines = buffer.split("\n");
        buffer = lines.pop() || "";

        for (const line of lines) {
          if (line.startsWith("data: ")) {
            const data = line.slice(6);
            if (data === "[DONE]") continue;
            try {
              const parsed = JSON.parse(data);
              if (parsed.type === "reasoning") {
                setMessages((prev) => {
                  const updated = [...prev];
                  const idx = updated.findIndex((m) => m.id === msgId);
                  if (idx >= 0) updated[idx].reasoning += parsed.content;
                  return updated;
                });
              } else if (parsed.type === "answer") {
                setMessages((prev) => {
                  const updated = [...prev];
                  const idx = updated.findIndex((m) => m.id === msgId);
                  if (idx >= 0) updated[idx].content += parsed.content;
                  return updated;
                });
              } else if (parsed.type === "done") {
                setMessages((prev) => {
                  const updated = [...prev];
                  const idx = updated.findIndex((m) => m.id === msgId);
                  if (idx >= 0) updated[idx].isStreaming = false;
                  return updated;
                });
              } else if (parsed.type === "error") {
                setMessages((prev) => {
                  const updated = [...prev];
                  const idx = updated.findIndex((m) => m.id === msgId);
                  if (idx >= 0) {
                    updated[idx].content = parsed.error || "Error";
                    updated[idx].isStreaming = false;
                  }
                  return updated;
                });
              }
            } catch {}
          }
        }
      }
    } catch {
      setMessages((prev) => {
        const updated = [...prev];
        const idx = updated.findIndex((m) => m.id === msgId);
        if (idx >= 0) {
          updated[idx].content = "后端暂未连接。可点击“演示登录”体验 EMOS 问答流程。";
          updated[idx].isStreaming = false;
        }
        return updated;
      });
    }

    setLoading(false);
  }, [input, loading, token, activeSession, fetchSessions, demoMode, streamDemoAnswer]);

  if (!loggedIn) {
    return (
      <main className="min-h-screen bg-[#f5f7fb] text-slate-950">
        <div className="mx-auto grid min-h-screen max-w-6xl grid-cols-1 items-center gap-10 px-6 py-10 md:grid-cols-[1.1fr_0.9fr]">
          <section>
            <div className="mb-5 inline-flex items-center gap-2 rounded-full border border-slate-200 bg-white px-3 py-1 text-sm text-slate-600 shadow-sm">
              <span className="h-2 w-2 rounded-full bg-emerald-500" />
              实验室内部知识库
            </div>
            <h1 className="text-5xl font-semibold tracking-tight text-slate-950">EMOS</h1>
            <p className="mt-4 max-w-xl text-lg leading-8 text-slate-600">
              面向实验室内部文档、论文和项目资料的知识问答系统，支持课题组数据隔离、混合检索和流式对话。
            </p>
            <div className="mt-8 grid max-w-2xl grid-cols-3 gap-3">
              {["FAISS + BM25", "SSE Streaming", "MySQL Quota"].map((item) => (
                <div key={item} className="rounded-lg border border-slate-200 bg-white px-4 py-3 text-sm font-medium text-slate-700 shadow-sm">
                  {item}
                </div>
              ))}
            </div>
          </section>

          <section className="rounded-xl border border-slate-200 bg-white p-6 shadow-xl">
            <h2 className="text-xl font-semibold text-slate-950">登录 EMOS</h2>
            <p className="mt-1 text-sm text-slate-500">登录后访问实验室知识库与个人对话记录。</p>
            {loginError && (
              <div className="mt-4 rounded-md bg-amber-50 px-3 py-2 text-sm text-amber-700">
                {loginError}
              </div>
            )}
            <input
              className="mt-5 w-full rounded-md border border-slate-300 px-3 py-2 outline-none focus:border-slate-500"
              placeholder="Username"
              value={username}
              onChange={(e) => setUsername(e.target.value)}
              onKeyDown={(e) => e.key === "Enter" && login()}
            />
            <input
              className="mt-3 w-full rounded-md border border-slate-300 px-3 py-2 outline-none focus:border-slate-500"
              type="password"
              placeholder="Password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
              onKeyDown={(e) => e.key === "Enter" && login()}
            />
            <button
              className="mt-5 w-full rounded-md bg-slate-950 px-4 py-2.5 text-white hover:bg-slate-800"
              onClick={login}
            >
              登录
            </button>
            <button
              className="mt-3 w-full rounded-md border border-slate-300 px-4 py-2.5 text-slate-700 hover:bg-slate-50"
              onClick={startDemo}
            >
              演示登录
            </button>
          </section>
        </div>
      </main>
    );
  }

  return (
    <div className="flex min-h-screen bg-[#f5f7fb] text-slate-950">
      {sidebarOpen && (
        <aside className="flex w-72 shrink-0 flex-col border-r border-slate-200 bg-slate-950 text-white">
          <div className="border-b border-white/10 p-5">
            <div className="text-2xl font-semibold">EMOS</div>
            <div className="mt-1 text-xs text-slate-400">实验室知识助手</div>
            <button
              className="mt-5 w-full rounded-md bg-white/10 px-3 py-2 text-sm hover:bg-white/15"
              onClick={createSession}
            >
              新建对话
            </button>
          </div>
          <div className="flex-1 overflow-y-auto p-3">
            {sessions.map((s) => (
              <div
                key={s.id}
                className={`group flex cursor-pointer items-center rounded-md px-3 py-2 hover:bg-white/10 ${
                  activeSession === s.id ? "bg-white/15" : ""
                }`}
                onClick={() => switchSession(s.id)}
              >
                <span className="flex-1 truncate text-sm">{s.title}</span>
                <button
                  className="hidden text-xs text-slate-400 hover:text-red-300 group-hover:block"
                  onClick={(e) => deleteSession(s.id, e)}
                >
                  x
                </button>
              </div>
            ))}
            {sessions.length === 0 && (
              <div className="mt-4 text-center text-xs text-slate-500">暂无会话</div>
            )}
          </div>
          <div className="border-t border-white/10 p-4">
            <div className="rounded-md bg-white/10 p-3 text-xs text-slate-300">
              <div>系统状态：正常</div>
              <div className="mt-1">访问控制：已启用</div>
            </div>
            <button
              className="mt-3 text-sm text-slate-400 hover:text-white"
              onClick={() => {
                setLoggedIn(false);
                setToken("");
                setDemoMode(false);
              }}
            >
              退出
            </button>
          </div>
        </aside>
      )}

      <div className="flex min-w-0 flex-1 flex-col">
        <header className="flex items-center justify-between border-b border-slate-200 bg-white px-5 py-3">
          <div className="flex items-center gap-3">
            <button
              className="rounded-md border border-slate-200 px-2 py-1 text-slate-600 hover:bg-slate-50"
              onClick={() => setSidebarOpen(!sidebarOpen)}
            >
              {sidebarOpen ? "<" : ">"}
            </button>
            <div>
              <h1 className="text-lg font-semibold">EMOS 知识助手</h1>
              <p className="text-xs text-slate-500">实验室文档、论文与项目知识库</p>
            </div>
          </div>
          <div className="hidden items-center gap-2 text-xs text-slate-500 md:flex">
            <span className="rounded-full bg-emerald-50 px-2 py-1 text-emerald-700">MySQL fact source</span>
            <span className="rounded-full bg-blue-50 px-2 py-1 text-blue-700">FAISS + BM25</span>
          </div>
        </header>

        <div className="mx-auto w-full max-w-4xl flex-1 overflow-y-auto px-5 py-5">
          {messages.length === 0 && (
            <div className="mt-16 rounded-xl border border-slate-200 bg-white p-8 text-center shadow-sm">
              <h2 className="text-2xl font-semibold">开始一次 EMOS 对话</h2>
              <p className="mt-3 text-slate-500">
                输入框已准备好问题，点击发送即可查询实验室知识库中的论文资料。
              </p>
            </div>
          )}
          {messages.map((msg) => (
            <div key={msg.id} className={`mb-5 flex ${msg.role === "user" ? "justify-end" : "justify-start"}`}>
              <div
                className={`max-w-[82%] rounded-xl px-4 py-3 shadow-sm ${
                  msg.role === "user"
                    ? "bg-slate-950 text-white"
                    : "border border-slate-200 bg-white text-slate-900"
                }`}
              >
                {msg.role === "assistant" && msg.reasoning && (
                  <div className="mb-3 rounded-md bg-slate-50 p-2">
                    <button
                      className="flex items-center gap-1 text-xs text-slate-500 hover:text-slate-700"
                      onClick={() =>
                        setShowReasoning((prev) => ({ ...prev, [msg.id]: !prev[msg.id] }))
                      }
                    >
                      <span>{showReasoning[msg.id] ? "v" : ">"}</span>
                      检索与推理摘要
                    </button>
                    {showReasoning[msg.id] && (
                      <div className="mt-2 whitespace-pre-wrap text-xs text-slate-500">
                        {msg.reasoning}
                      </div>
                    )}
                  </div>
                )}
                {msg.role === "assistant" ? (
                  <div className="prose prose-sm max-w-none prose-slate">
                    <ReactMarkdown remarkPlugins={[remarkGfm]}>
                      {msg.content}
                    </ReactMarkdown>
                    {!msg.isStreaming && demoMode && msg.content.includes("FlowDyn") && (
                      <PaperDownloadCard />
                    )}
                  </div>
                ) : (
                  <span>{msg.content}</span>
                )}
                {msg.isStreaming && (
                  <span className="ml-1 inline-block h-4 w-2 animate-pulse bg-slate-400" />
                )}
              </div>
            </div>
          ))}
          <div ref={messagesEndRef} />
        </div>

        <div className="border-t border-slate-200 bg-white p-4">
          <div className="mx-auto flex max-w-4xl gap-2">
            <input
              className="flex-1 rounded-md border border-slate-300 px-3 py-2 outline-none focus:border-slate-500"
              placeholder="向 EMOS 提问..."
              value={input}
              onChange={(e) => setInput(e.target.value)}
              onKeyDown={(e) => e.key === "Enter" && !e.shiftKey && sendMessage()}
              disabled={loading}
            />
            <button
              className="rounded-md bg-slate-950 px-5 py-2 text-white hover:bg-slate-800 disabled:opacity-50"
              onClick={sendMessage}
              disabled={loading || !input.trim()}
            >
              {loading ? "生成中" : "发送"}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
