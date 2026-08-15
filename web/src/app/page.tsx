"use client";

import { useState, useCallback, useRef, useEffect } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";

interface Message {
  id: string;
  role: "user" | "assistant";
  content: string;
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
export default function Home() {
  const [messages, setMessages] = useState<Message[]>([]);
  const [input, setInput] = useState("");
  const [loading, setLoading] = useState(false);
  const [token, setToken] = useState("");
  const [loggedIn, setLoggedIn] = useState(false);
  const [username, setUsername] = useState("admin");
  const [password, setPassword] = useState("");
  const [loginError, setLoginError] = useState("");
  const [sessions, setSessions] = useState<Session[]>([]);
  const [activeSession, setActiveSession] = useState<string>("");
  const messagesEndRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [messages]);

  const fetchSessions = useCallback(async () => {
    if (!token) return;
    try {
      const res = await fetch(`${API_BASE}/api/sessions`, {
        headers: { Authorization: `Bearer ${token}` },
      });
      const data = await res.json();
      setSessions(data.sessions || []);
    } catch {}
  }, [token]);

  useEffect(() => {
    if (!loggedIn || !token) return;

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
  }, [loggedIn, token]);

  const createSession = useCallback(async () => {
    try {
      const res = await fetch(`${API_BASE}/api/sessions`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${token}`,
        },
        body: JSON.stringify({ title: "新对话" }),
      });
      const data = await res.json();
      if (data.id) {
        setActiveSession(data.id);
        setMessages([]);
        fetchSessions();
      }
    } catch {}
  }, [token, fetchSessions]);

  const switchSession = useCallback(async (id: string) => {
    setActiveSession(id);
    setMessages([]);

    try {
      const res = await fetch(`${API_BASE}/api/sessions/${id}`, {
        headers: { Authorization: `Bearer ${token}` },
      });
      const data = await res.json();
      if (data.messages) {
        const loaded: Message[] = data.messages.map(
          (m: { id: number; role: string; content: string }) => ({
            id: String(m.id),
            role: m.role as "user" | "assistant",
            content: m.content || "",
            isStreaming: false,
          })
        );
        setMessages(loaded);
      }
    } catch {}
  }, [token]);

  const deleteSession = useCallback(
    async (id: string, e: React.MouseEvent) => {
      e.stopPropagation();
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
    [token, activeSession, fetchSessions]
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
        setLoginError("");
      } else {
        setLoginError(data.error || "登录失败");
      }
    } catch {
      setLoginError("暂时无法登录，请稍后再试。");
    }
  }, [username, password]);

  const sendMessage = useCallback(async () => {
    if (!input.trim() || loading) return;

    let sid = activeSession;
    if (!sid) {
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

    const userMsg: Message = {
      id: Date.now().toString(),
      role: "user",
      content: input,
      isStreaming: false,
    };
    setMessages((prev) => [...prev, userMsg]);

    const msgId = (Date.now() + 1).toString();
    const assistantMsg: Message = {
      id: msgId,
      role: "assistant",
      content: "",
      isStreaming: true,
    };
    setMessages((prev) => [...prev, assistantMsg]);
    setInput("");
    setLoading(true);

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
              if (parsed.type === "answer") {
                setMessages((prev) =>
                  prev.map((message) =>
                    message.id === msgId
                      ? { ...message, content: message.content + parsed.content }
                      : message
                  )
                );
              } else if (parsed.type === "done") {
                setMessages((prev) =>
                  prev.map((message) =>
                    message.id === msgId ? { ...message, isStreaming: false } : message
                  )
                );
              } else if (parsed.type === "error") {
                setMessages((prev) =>
                  prev.map((message) =>
                    message.id === msgId
                      ? {
                          ...message,
                          content: parsed.error || "Error",
                          isStreaming: false,
                        }
                      : message
                  )
                );
              }
            } catch {}
          }
        }
      }
    } catch {
      setMessages((prev) =>
        prev.map((message) =>
          message.id === msgId
            ? {
                ...message,
                content: "暂时无法生成答案，请稍后再试。",
                isStreaming: false,
              }
            : message
        )
      );
    }

    setLoading(false);
  }, [input, loading, token, activeSession, fetchSessions]);

  if (!loggedIn) {
    return (
      <main className="min-h-screen bg-[#f7f8fa] text-slate-950">
        <div className="mx-auto flex min-h-screen max-w-md items-center px-6 py-10">
          <section className="w-full">
            <div className="mb-10 text-center">
              <div className="mx-auto flex h-12 w-12 items-center justify-center rounded-2xl bg-slate-950 text-sm font-semibold tracking-[0.2em] text-white">
                E
              </div>
              <h1 className="mt-5 text-3xl font-semibold tracking-tight">EMOS</h1>
              <p className="mt-2 text-sm text-slate-500">内部资料问答</p>
            </div>

            <section className="rounded-2xl border border-slate-200/80 bg-white p-6 shadow-[0_16px_50px_rgba(15,23,42,0.08)] sm:p-8">
              <h2 className="text-xl font-semibold text-slate-950">登录</h2>
              {loginError && (
                <div className="mt-4 rounded-lg bg-amber-50 px-3 py-2 text-sm text-amber-700">
                  {loginError}
                </div>
              )}
              <label className="mt-6 block text-sm font-medium text-slate-700">
                用户名
                <input
                  className="mt-2 w-full rounded-lg border border-slate-300 px-3 py-2.5 outline-none transition focus:border-slate-500 focus:ring-2 focus:ring-slate-200"
                  placeholder="请输入用户名"
                  value={username}
                  onChange={(e) => setUsername(e.target.value)}
                  onKeyDown={(e) => e.key === "Enter" && login()}
                />
              </label>
              <label className="mt-4 block text-sm font-medium text-slate-700">
                密码
                <input
                  className="mt-2 w-full rounded-lg border border-slate-300 px-3 py-2.5 outline-none transition focus:border-slate-500 focus:ring-2 focus:ring-slate-200"
                  type="password"
                  placeholder="请输入密码"
                  value={password}
                  onChange={(e) => setPassword(e.target.value)}
                  onKeyDown={(e) => e.key === "Enter" && login()}
                />
              </label>
              <button
                className="mt-6 w-full rounded-lg bg-slate-950 px-4 py-2.5 font-medium text-white transition hover:bg-slate-800"
                onClick={login}
              >
                登录
              </button>
            </section>
          </section>
        </div>
      </main>
    );
  }

  return (
    <div className="flex min-h-screen bg-[#f5f7fb] text-slate-950">
      <aside className="flex w-72 shrink-0 flex-col border-r border-slate-200 bg-slate-950 text-white">
        <div className="border-b border-white/10 p-5">
          <div className="text-2xl font-semibold tracking-tight">EMOS</div>
          <button
            className="mt-5 w-full rounded-lg bg-white px-3 py-2.5 text-sm font-medium text-slate-950 transition hover:bg-slate-200"
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
          <button
            className="text-sm text-slate-400 transition hover:text-white"
            onClick={() => {
              setLoggedIn(false);
              setToken("");
            }}
          >
            退出
          </button>
        </div>
      </aside>

      <div className="flex min-w-0 flex-1 flex-col">
        <header className="flex items-center justify-between border-b border-slate-200 bg-white px-5 py-4">
          <div>
            <h1 className="text-lg font-semibold">对话</h1>
          </div>
        </header>

        <div className="mx-auto w-full max-w-4xl flex-1 overflow-y-auto px-5 py-5">
          {messages.length === 0 && (
            <div className="mt-16 rounded-2xl border border-slate-200 bg-white p-8 text-center shadow-sm">
              <h2 className="text-2xl font-semibold">暂无消息</h2>
              <p className="mt-3 text-slate-500">输入问题开始对话。</p>
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
                {msg.role === "assistant" ? (
                  <div className="prose prose-sm max-w-none prose-slate">
                    <ReactMarkdown remarkPlugins={[remarkGfm]}>
                      {msg.content}
                    </ReactMarkdown>
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
              className="flex-1 rounded-lg border border-slate-300 px-3 py-2.5 outline-none transition focus:border-slate-500 focus:ring-2 focus:ring-slate-200"
              placeholder="输入问题"
              value={input}
              onChange={(e) => setInput(e.target.value)}
              onKeyDown={(e) => e.key === "Enter" && !e.shiftKey && sendMessage()}
              disabled={loading}
            />
            <button
              className="rounded-lg bg-slate-950 px-5 py-2.5 font-medium text-white transition hover:bg-slate-800 disabled:opacity-50"
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
