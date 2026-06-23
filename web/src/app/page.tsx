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

export default function Home() {
  const [messages, setMessages] = useState<Message[]>([]);
  const [input, setInput] = useState("");
  const [loading, setLoading] = useState(false);
  const [token, setToken] = useState("");
  const [loggedIn, setLoggedIn] = useState(false);
  const [username, setUsername] = useState("");
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
    if (loggedIn) fetchSessions();
  }, [loggedIn, fetchSessions]);

  const createSession = useCallback(async () => {
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
        setLoginError(data.error || "Login failed");
      }
    } catch {
      setLoginError("Network error");
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
      reasoning: "",
      isStreaming: false,
    };
    setMessages((prev) => [...prev, userMsg]);
    const msgId = (Date.now() + 1).toString();
    const assistantMsg: Message = {
      id: msgId,
      role: "assistant",
      content: "",
      reasoning: "",
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
          updated[idx].content = "Network error";
          updated[idx].isStreaming = false;
        }
        return updated;
      });
    }

    setLoading(false);
  }, [input, loading, token, activeSession, fetchSessions]);

  if (!loggedIn) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gray-50">
        <div className="bg-white p-8 rounded-lg shadow-md w-96">
          <h1 className="text-2xl font-bold mb-2 text-center">Fast LLM Reasoner</h1>
          <p className="text-sm text-gray-500 mb-6 text-center">
            Enterprise Private LLM Platform
          </p>
          {loginError && (
            <div className="bg-red-50 text-red-600 p-2 rounded mb-4 text-sm">
              {loginError}
            </div>
          )}
          <input
            className="w-full border rounded px-3 py-2 mb-3"
            placeholder="Username"
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && login()}
          />
          <input
            className="w-full border rounded px-3 py-2 mb-4"
            type="password"
            placeholder="Password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && login()}
          />
          <button
            className="w-full bg-blue-600 text-white py-2 rounded hover:bg-blue-700"
            onClick={login}
          >
            Login
          </button>
        </div>
      </div>
    );
  }

  return (
    <div className="min-h-screen flex bg-gray-50">
      {sidebarOpen && (
        <div className="w-64 bg-gray-900 text-white flex flex-col">
          <div className="p-3 border-b border-gray-700">
            <button
              className="w-full bg-gray-700 hover:bg-gray-600 text-sm py-2 px-3 rounded"
              onClick={createSession}
            >
              + New Chat
            </button>
          </div>
          <div className="flex-1 overflow-y-auto">
            {sessions.map((s) => (
              <div
                key={s.id}
                className={`group flex items-center px-3 py-2 cursor-pointer hover:bg-gray-800 ${
                  activeSession === s.id ? "bg-gray-700" : ""
                }`}
                onClick={() => switchSession(s.id)}
              >
                <span className="flex-1 text-sm truncate">{s.title}</span>
                <button
                  className="hidden group-hover:block text-gray-400 hover:text-red-400 text-xs ml-1"
                  onClick={(e) => deleteSession(s.id, e)}
                >
                  x
                </button>
              </div>
            ))}
            {sessions.length === 0 && (
              <div className="text-gray-500 text-xs text-center mt-4">No sessions yet</div>
            )}
          </div>
          <div className="p-3 border-t border-gray-700">
            <button
              className="text-sm text-gray-400 hover:text-white"
              onClick={() => {
                setLoggedIn(false);
                setToken("");
              }}
            >
              Logout
            </button>
          </div>
        </div>
      )}

      <div className="flex-1 flex flex-col">
        <header className="bg-white border-b px-4 py-3 flex items-center gap-3">
          <button
            className="text-gray-500 hover:text-gray-700"
            onClick={() => setSidebarOpen(!sidebarOpen)}
          >
            {sidebarOpen ? "\u25C4" : "\u25B6"}
          </button>
          <h1 className="font-bold text-lg">Fast LLM Reasoner</h1>
        </header>

        <div className="flex-1 overflow-y-auto p-4 max-w-3xl mx-auto w-full">
          {messages.length === 0 && (
            <div className="text-center text-gray-400 mt-20">
              <p className="text-xl mb-2">Start a conversation</p>
              <p className="text-sm">Ask questions to the local LLM</p>
            </div>
          )}
          {messages.map((msg) => (
            <div
              key={msg.id}
              className={`mb-4 ${msg.role === "user" ? "text-right" : "text-left"}`}
            >
              <div
                className={`inline-block max-w-[80%] p-3 rounded-lg ${
                  msg.role === "user"
                    ? "bg-blue-600 text-white"
                    : "bg-white border shadow-sm"
                }`}
              >
                {msg.role === "assistant" && msg.reasoning && (
                  <div className="mb-2">
                    <button
                      className="text-xs text-gray-500 hover:text-gray-700 flex items-center gap-1"
                      onClick={() =>
                        setShowReasoning((prev) => ({ ...prev, [msg.id]: !prev[msg.id] }))
                      }
                    >
                      <span>{showReasoning[msg.id] ? "\u25BC" : "\u25B6"}</span>
                      Reasoning
                    </button>
                    {showReasoning[msg.id] && (
                      <div className="text-xs text-gray-500 bg-gray-50 p-2 rounded mt-1 whitespace-pre-wrap">
                        {msg.reasoning}
                      </div>
                    )}
                  </div>
                )}
                {msg.role === "assistant" ? (
                  <div className="prose prose-sm max-w-none">
                    <ReactMarkdown remarkPlugins={[remarkGfm]}>
                      {msg.content}
                    </ReactMarkdown>
                  </div>
                ) : (
                  <span>{msg.content}</span>
                )}
                {msg.isStreaming && (
                  <span className="inline-block w-2 h-4 bg-gray-400 animate-pulse ml-1" />
                )}
              </div>
            </div>
          ))}
          <div ref={messagesEndRef} />
        </div>

        <div className="border-t bg-white p-4">
          <div className="max-w-3xl mx-auto flex gap-2">
            <input
              className="flex-1 border rounded px-3 py-2 focus:outline-none focus:ring-2 focus:ring-blue-500"
              placeholder="Type a message..."
              value={input}
              onChange={(e) => setInput(e.target.value)}
              onKeyDown={(e) => e.key === "Enter" && !e.shiftKey && sendMessage()}
              disabled={loading}
            />
            <button
              className="bg-blue-600 text-white px-4 py-2 rounded hover:bg-blue-700 disabled:opacity-50"
              onClick={sendMessage}
              disabled={loading || !input.trim()}
            >
              {loading ? "..." : "Send"}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
