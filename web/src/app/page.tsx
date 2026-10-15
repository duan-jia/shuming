"use client";

import { useState, useCallback, useRef, useEffect } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";

interface Message {
  id: string;
  role: "user" | "assistant";
  content: string;
  isStreaming: boolean;
  isReasoning?: boolean;
}

interface Session {
  id: string;
  title: string;
  model: string;
  created_at: string;
  updated_at: string;
}

interface DocumentItem {
  id: number;
  tenant_id: number;
  filename: string;
  source_filename: string | null;
  review_reason: string | null;
  source_size_bytes: number;
  markdown_size_bytes: number;
  chunk_count: number;
  visibility: "group" | "public";
  status: "processing" | "pending" | "ready" | "failed";
  uploaded_by: number;
  uploader_username: string;
  has_pdf: boolean;
  can_delete: boolean;
  created_at: string;
  updated_at: string;
}

function formatBytes(bytes: number) {
  if (!bytes) return "—";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function formatDate(value: string) {
  if (!value) return "—";
  const date = new Date(value.replace(" ", "T"));
  return Number.isNaN(date.getTime()) ? value : date.toLocaleString("zh-CN");
}

function createRequestId() {
  try {
    if (globalThis.isSecureContext && typeof globalThis.crypto?.randomUUID === "function") {
      return globalThis.crypto.randomUUID();
    }
  } catch {}
  return `web-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

const documentStatus: Record<DocumentItem["status"], { label: string; className: string }> = {
  processing: { label: "处理中", className: "bg-blue-50 text-blue-700" },
  pending: { label: "待审核", className: "bg-amber-50 text-amber-700" },
  ready: { label: "可用", className: "bg-emerald-50 text-emerald-700" },
  failed: { label: "失败", className: "bg-rose-50 text-rose-700" },
};

const API_BASE = "";
const EMPTY_MESSAGES: Message[] = [];
export default function Home() {
  const [sessionMessages, setSessionMessages] = useState<Record<string, Message[]>>({});
  const [input, setInput] = useState("");
  const [generating, setGenerating] = useState<Record<string, boolean>>({});
  const [token, setToken] = useState("");
  const [loggedIn, setLoggedIn] = useState(false);
  const [userRole, setUserRole] = useState<"admin" | "user">("user");
  const [username, setUsername] = useState("admin");
  const [password, setPassword] = useState("");
  const [loginError, setLoginError] = useState("");
  const [sessions, setSessions] = useState<Session[]>([]);
  const [activeSession, setActiveSession] = useState<string>("");
  const messages = sessionMessages[activeSession] || EMPTY_MESSAGES;
  const loading = !!generating[activeSession];
  const requests = useRef(new Map<string, AbortController>());
  const historyVersions = useRef(new Map<string, number>());
  const navigationVersion = useRef(0);
  const historySequence = useRef(0);
  const activeSessionRef = useRef(activeSession);
  const [activeView, setActiveView] = useState<"chat" | "documents">("chat");
  const [documents, setDocuments] = useState<DocumentItem[]>([]);
  const [documentsLoading, setDocumentsLoading] = useState(false);
  const [documentsError, setDocumentsError] = useState("");
  const [deletingDocument, setDeletingDocument] = useState<number | null>(null);
  const [uploading, setUploading] = useState(false);
  const [uploadNotice, setUploadNotice] = useState<{
    type: "success" | "error";
    message: string;
  } | null>(null);
  const messagesEndRef = useRef<HTMLDivElement>(null);
  const pdfInputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [messages]);

  useEffect(() => {
    const pending = requests.current;
    return () => {
      for (const controller of pending.values()) controller.abort();
      pending.clear();
    };
  }, []);

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

  const fetchDocuments = useCallback(async (showLoading = false) => {
    if (!token) return;
    if (showLoading) setDocumentsLoading(true);
    try {
      const res = await fetch(`${API_BASE}/api/documents`, {
        headers: { Authorization: `Bearer ${token}` },
        cache: "no-store",
      });
      const data = await res.json().catch(() => ({}));
      if (!res.ok) throw new Error(data.error || "无法加载文档列表。");
      setDocuments(data.documents || []);
      setDocumentsError("");
    } catch (error) {
      setDocumentsError(error instanceof Error ? error.message : "无法加载文档列表。");
    } finally {
      if (showLoading) setDocumentsLoading(false);
    }
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

  useEffect(() => {
    if (activeView !== "documents" || !documents.some((doc) => doc.status === "processing")) {
      return;
    }
    const timer = window.setInterval(() => void fetchDocuments(), 3000);
    return () => window.clearInterval(timer);
  }, [activeView, documents, fetchDocuments]);

  const createSession = useCallback(async () => {
    const navigation = ++navigationVersion.current;
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
      if (data.id && navigation === navigationVersion.current) {
        activeSessionRef.current = data.id;
        setActiveView("chat");
        setActiveSession(data.id);
        setSessionMessages((prev) => ({ ...prev, [data.id]: [] }));
        fetchSessions();
      }
    } catch {}
  }, [token, fetchSessions]);

  const switchSession = useCallback(async (id: string) => {
    ++navigationVersion.current;
    activeSessionRef.current = id;
    setActiveView("chat");
    setActiveSession(id);
    // In-flight replies are authoritative; do not replace them with DB history.
    if (requests.current.has(id)) return;
    const version = ++historySequence.current;
    historyVersions.current.set(id, version);

    try {
      const res = await fetch(`${API_BASE}/api/sessions/${id}`, {
        headers: { Authorization: `Bearer ${token}` },
      });
      const data = await res.json();
      if (data.messages && historyVersions.current.get(id) === version && !requests.current.has(id)) {
        const loaded: Message[] = data.messages.map(
          (m: { id: number; role: string; content: string }) => ({
            id: String(m.id),
            role: m.role as "user" | "assistant",
            content: m.content || "",
            isStreaming: false,
          })
        );
        setSessionMessages((prev) => ({ ...prev, [id]: loaded }));
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
        historyVersions.current.set(id, ++historySequence.current);
        requests.current.get(id)?.abort();
        requests.current.delete(id);
        setGenerating((prev) => ({ ...prev, [id]: false }));
        setSessionMessages((prev) => {
          const next = { ...prev };
          delete next[id];
          return next;
        });
        if (activeSessionRef.current === id) {
          ++navigationVersion.current;
          activeSessionRef.current = "";
          setActiveSession("");
        }
        fetchSessions();
      } catch {}
    },
    [token, fetchSessions]
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
        setUserRole(data.user?.role === "admin" ? "admin" : "user");
        setLoggedIn(true);
        setLoginError("");
      } else {
        setLoginError(data.error || "登录失败");
      }
    } catch {
      setLoginError("暂时无法登录，请稍后再试。");
    }
  }, [username, password]);

  const uploadPdf = useCallback(
    async (event: React.ChangeEvent<HTMLInputElement>) => {
      const file = event.target.files?.[0];
      event.target.value = "";
      if (!file || uploading) return;

      if (!file.name.toLowerCase().endsWith(".pdf") || file.size === 0 || file.size > 30 * 1024 * 1024) {
        setUploadNotice({ type: "error", message: "请选择一个小于 30 MB 的 PDF 文件。" });
        return;
      }

      setUploading(true);
      setUploadNotice(null);

      try {
        const formData = new FormData();
        formData.append("file", file);
        const response = await fetch("/api/papers/upload", {
          method: "POST",
          headers: { Authorization: `Bearer ${token}` },
          body: formData,
        });
        const data = await response.json();
        if (!response.ok) {
          throw new Error(data.error || "PDF 上传失败。");
        }
        setUploadNotice({
          type: "success",
          message: data.status === "pending"
            ? `${data.filename} 已转换为 Markdown，并进入待审核队列（文档 #${data.document_id}）。`
            : `${data.filename} 已压缩为 Markdown，并已提交 RAG（文档 #${data.document_id}，状态：${data.status}）。`,
        });
        await fetchDocuments();
      } catch (error) {
        setUploadNotice({
          type: "error",
          message: error instanceof Error ? error.message : "PDF 上传失败。",
        });
      } finally {
        setUploading(false);
      }
    },
    [token, uploading, fetchDocuments]
  );

  const downloadDocument = useCallback(async (document: DocumentItem, format: "pdf" | "md") => {
    try {
      const response = await fetch(`/api/documents/${document.id}/download/${format}`, {
        headers: { Authorization: `Bearer ${token}` },
      });
      if (!response.ok) {
        const data = await response.json().catch(() => ({}));
        throw new Error(data.error || "下载失败。");
      }
      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      const anchor = window.document.createElement("a");
      anchor.href = url;
      anchor.download = format === "pdf"
        ? document.source_filename || `document-${document.id}.pdf`
        : document.filename;
      anchor.click();
      URL.revokeObjectURL(url);
    } catch (error) {
      setUploadNotice({
        type: "error",
        message: error instanceof Error ? error.message : "下载失败。",
      });
    }
  }, [token]);

  const approveDocument = useCallback(async (document: DocumentItem) => {
    try {
      const response = await fetch(`${API_BASE}/api/documents/${document.id}/approve`, {
        method: "POST",
        headers: { Authorization: `Bearer ${token}` },
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(data.error || "审核通过失败。");
      setUploadNotice({ type: "success", message: `${document.filename} 已通过审核并提交 RAG。` });
      await fetchDocuments();
    } catch (error) {
      setUploadNotice({
        type: "error",
        message: error instanceof Error ? error.message : "审核通过失败。",
      });
    }
  }, [token, fetchDocuments]);

  const deleteDocument = useCallback(async (document: DocumentItem) => {
    const displayName = document.source_filename || document.filename;
    if (!window.confirm(`确定永久删除“${displayName}”及其磁盘文件吗？此操作无法恢复。`)) {
      return;
    }
    setDeletingDocument(document.id);
    setUploadNotice(null);
    try {
      const response = await fetch(`/api/documents/${document.id}`, {
        method: "DELETE",
        headers: { Authorization: `Bearer ${token}` },
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(data.error || "删除文档失败。");
      setDocuments((current) => current.filter((item) => item.id !== document.id));
      setUploadNotice({
        type: data.cleanup_warning ? "error" : "success",
        message: data.cleanup_warning || `“${displayName}”已删除。`,
      });
    } catch (error) {
      setUploadNotice({
        type: "error",
        message: error instanceof Error ? error.message : "删除文档失败。",
      });
    } finally {
      setDeletingDocument(null);
    }
  }, [token]);

  const sendMessage = useCallback(async () => {
    if (!input.trim() || requests.current.has(activeSession)) return;
    const controller = new AbortController();
    const navigation = navigationVersion.current;
    requests.current.set(activeSession, controller);
    setGenerating((prev) => ({ ...prev, [activeSession]: true }));

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
          signal: controller.signal,
        });
        const data = await res.json();
        if (requests.current.get("") !== controller) return;
        if (!res.ok || !data.id) throw new Error("无法创建会话");
        if (data.id) {
          sid = data.id;
          requests.current.delete("");
          requests.current.set(sid, controller);
          setGenerating((prev) => ({ ...prev, "": false, [sid]: true }));
          if (navigation === navigationVersion.current) {
            activeSessionRef.current = sid;
            setActiveSession(sid);
          }
          fetchSessions();
        }
      } catch {
        if (requests.current.get("") === controller) {
          requests.current.delete("");
          setGenerating((prev) => ({ ...prev, "": false }));
        }
        return;
      }
    }

    historyVersions.current.set(sid, ++historySequence.current);
    const setMessages = (update: (messages: Message[]) => Message[]) => {
      if (requests.current.get(sid) !== controller) return;
      setSessionMessages((prev) => ({ ...prev, [sid]: update(prev[sid] || []) }));
    };
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
    if (navigation === navigationVersion.current) setInput("");

    try {
      const res = await fetch(`${API_BASE}/api/chat/stream`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${token}`,
          "X-Request-Id": createRequestId(),
        },
        body: JSON.stringify({ message: userMsg.content, session_id: sid }),
        signal: controller.signal,
      });

      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const reader = res.body?.getReader();
      if (!reader) throw new Error("No reader");

      const decoder = new TextDecoder();
      let buffer = "";
      let eventType = "message";

      let finished = false;
      try {
        while (!finished) {
          const { done, value } = await reader.read();
          if (done) break;
          buffer += decoder.decode(value, { stream: true });

          const lines = buffer.split("\n");
          buffer = lines.pop() || "";

          for (const line of lines) {
            if (line.startsWith("event: ")) {
              eventType = line.slice(7);
            } else if (line.startsWith("data: ")) {
              const data = line.slice(6);
              if (data === "[DONE]") { finished = true; break; }
              try {
                const parsed = JSON.parse(data);
                if (eventType === "error" || parsed.type === "error" || parsed.error) {
                  finished = true;
                  setMessages((prev) =>
                    prev.map((message) =>
                      message.id === msgId
                        ? {
                            ...message,
                            content: parsed.error || "Error",
                            isStreaming: false,
                            isReasoning: false,
                          }
                        : message
                    )
                  );
                } else if (parsed.type === "answer") {
                  setMessages((prev) =>
                    prev.map((message) =>
                      message.id === msgId
                        ? {
                            ...message,
                            content: message.content + parsed.content,
                            isReasoning: false,
                          }
                        : message
                    )
                  );
                } else if (parsed.type === "reasoning") {
                  setMessages((prev) =>
                    prev.map((message) =>
                      message.id === msgId ? { ...message, isReasoning: true } : message
                    )
                  );
                } else if (parsed.type === "done") {
                  finished = true;
                  setMessages((prev) =>
                    prev.map((message) =>
                      message.id === msgId
                        ? { ...message, isStreaming: false, isReasoning: false }
                        : message
                    )
                  );
                }
              } catch {}
              if (finished) break;
            } else if (line === "") {
              eventType = "message";
            }
          }
        }
      } finally {
        await reader.cancel().catch(() => {});
        reader.releaseLock();
      }
    } catch (error) {
      if (controller.signal.aborted) return;
      const errorMessage =
        error instanceof Error && error.message
          ? `请求失败：${error.message}`
          : "暂时无法生成答案，请稍后再试。";
      setMessages((prev) =>
        prev.map((message) =>
          message.id === msgId
            ? {
                ...message,
                content: errorMessage,
                isStreaming: false,
              }
            : message
        )
      );
    } finally {
      setMessages((prev) => prev.map((message) =>
        message.id === msgId ? { ...message, isStreaming: false, isReasoning: false } : message
      ));
      if (requests.current.get(sid) === controller) {
        requests.current.delete(sid);
        setGenerating((prev) => ({ ...prev, [sid]: false }));
      }
    }
  }, [input, token, activeSession, fetchSessions]);

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
      <aside className="flex w-56 shrink-0 flex-col border-r border-slate-200 bg-slate-950 text-white sm:w-72">
        <div className="border-b border-white/10 p-5">
          <div className="text-2xl font-semibold tracking-tight">EMOS</div>
          <button
            className="mt-5 w-full rounded-lg bg-white px-3 py-2.5 text-sm font-medium text-slate-950 transition hover:bg-slate-200"
            onClick={createSession}
          >
            新建对话
          </button>
          <div className="mt-3 grid grid-cols-2 gap-2">
            <button
              className={`rounded-lg px-3 py-2 text-sm transition ${
                activeView === "chat" ? "bg-white/15 text-white" : "text-slate-400 hover:bg-white/10 hover:text-white"
              }`}
              onClick={() => {
                setActiveView("chat");
                setUploadNotice(null);
              }}
            >
              对话
            </button>
            <button
              className={`rounded-lg px-3 py-2 text-sm transition ${
                activeView === "documents" ? "bg-white/15 text-white" : "text-slate-400 hover:bg-white/10 hover:text-white"
              }`}
              onClick={() => {
                setActiveView("documents");
                setUploadNotice(null);
                void fetchDocuments(true);
              }}
            >
              文档管理
            </button>
          </div>
        </div>
        <div className="flex-1 overflow-y-auto p-3">
          {activeView === "chat" ? (
            <>
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
            </>
          ) : (
            <div className="rounded-lg border border-white/10 bg-white/5 p-3 text-xs leading-5 text-slate-400">
              查看本课题组和公共知识库中的文档，并管理自己上传的资料。
            </div>
          )}
        </div>
        <div className="border-t border-white/10 p-4">
          <button
            className="text-sm text-slate-400 transition hover:text-white"
            onClick={() => {
              ++navigationVersion.current;
              for (const controller of requests.current.values()) controller.abort();
              requests.current.clear();
              historyVersions.current.clear();
              activeSessionRef.current = "";
              setActiveSession("");
              setSessionMessages({});
              setGenerating({});
              setInput("");
              setLoggedIn(false);
              setToken("");
              setUserRole("user");
            }}
          >
            退出
          </button>
        </div>
      </aside>

      <div className="flex min-w-0 flex-1 flex-col">
        <header className="flex items-center justify-between border-b border-slate-200 bg-white px-5 py-4">
          <div>
            <h1 className="text-lg font-semibold">{activeView === "chat" ? "对话" : "文档管理"}</h1>
            {activeView === "documents" && (
              <p className="mt-0.5 text-xs text-slate-500">PDF 原文与 RAG Markdown 索引统一管理</p>
            )}
          </div>
          {activeView === "documents" && (
            <div>
              <input
                ref={pdfInputRef}
                className="hidden"
                type="file"
                accept="application/pdf,.pdf"
                onChange={uploadPdf}
              />
              <button
                className="rounded-lg bg-slate-950 px-4 py-2 text-sm font-medium text-white transition hover:bg-slate-800 disabled:cursor-not-allowed disabled:opacity-50"
                type="button"
                onClick={() => pdfInputRef.current?.click()}
                disabled={uploading}
              >
                {uploading ? "上传并处理中" : "上传 PDF"}
              </button>
            </div>
          )}
        </header>

        <div className={`mx-auto w-full flex-1 overflow-y-auto px-5 py-5 ${activeView === "chat" ? "max-w-4xl" : "max-w-7xl"}`}>
          {uploadNotice && (
            <div
              className={`mb-5 rounded-lg border px-4 py-3 text-sm ${
                uploadNotice.type === "success"
                  ? "border-emerald-200 bg-emerald-50 text-emerald-700"
                  : "border-amber-200 bg-amber-50 text-amber-700"
              }`}
              role="status"
            >
              {uploadNotice.message}
            </div>
          )}
          {activeView === "chat" ? (
            <>
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
                        {msg.isReasoning && !msg.content ? (
                          <span className="text-slate-500">正在思考…</span>
                        ) : (
                          <ReactMarkdown remarkPlugins={[remarkGfm]}>
                            {msg.content}
                          </ReactMarkdown>
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
            </>
          ) : (
            <section>
              <div className="mb-4 flex items-center justify-between">
                <p className="text-sm text-slate-500">共 {documents.length} 条可见文档</p>
                <button
                  className="text-sm font-medium text-slate-600 transition hover:text-slate-950 disabled:opacity-50"
                  onClick={() => void fetchDocuments(true)}
                  disabled={documentsLoading}
                >
                  {documentsLoading ? "刷新中" : "刷新"}
                </button>
              </div>

              {documentsError && (
                <div className="mb-4 rounded-lg border border-amber-200 bg-amber-50 px-4 py-3 text-sm text-amber-700">
                  {documentsError}
                </div>
              )}

              {documentsLoading && documents.length === 0 ? (
                <div className="rounded-2xl border border-slate-200 bg-white p-10 text-center text-sm text-slate-500 shadow-sm">
                  正在加载文档…
                </div>
              ) : documents.length === 0 ? (
                <div className="rounded-2xl border border-dashed border-slate-300 bg-white p-10 text-center shadow-sm">
                  <h2 className="text-lg font-semibold">暂无可见文档</h2>
                  <p className="mt-2 text-sm text-slate-500">上传 PDF 后，系统会生成 Markdown 并加入 RAG。</p>
                </div>
              ) : (
                <>
                  <div className="hidden overflow-hidden rounded-xl border border-slate-200 bg-white shadow-sm md:block">
                    <table className="w-full table-fixed text-left text-sm">
                      <thead className="border-b border-slate-200 bg-slate-50 text-xs font-medium uppercase tracking-wide text-slate-500">
                        <tr>
                          <th className="w-[34%] px-4 py-3">文件</th>
                          <th className="w-[12%] px-4 py-3">状态</th>
                          <th className="w-[13%] px-4 py-3">范围</th>
                          <th className="w-[14%] px-4 py-3">上传者</th>
                          <th className="w-[15%] px-4 py-3">创建时间</th>
                          <th className="w-[12%] px-4 py-3 text-right">操作</th>
                        </tr>
                      </thead>
                      <tbody className="divide-y divide-slate-100">
                        {documents.map((doc) => (
                          <tr key={doc.id} className="align-top hover:bg-slate-50/70">
                            <td className="px-4 py-4">
                              <div className="min-w-0">
                                <div className="flex items-center gap-2">
                                  <span className="rounded bg-rose-50 px-1.5 py-0.5 text-[10px] font-semibold text-rose-700">PDF</span>
                                  <span className="truncate font-medium" title={doc.source_filename || "无源 PDF"}>
                                    {doc.source_filename || "历史记录无源 PDF"}
                                  </span>
                                  <span className="shrink-0 text-xs text-slate-400">{formatBytes(doc.source_size_bytes)}</span>
                                </div>
                                <div className="mt-2 flex items-center gap-2 text-slate-600">
                                  <span className="rounded bg-slate-100 px-1.5 py-0.5 text-[10px] font-semibold text-slate-600">MD</span>
                                  <span className="truncate" title={doc.filename}>{doc.filename}</span>
                                  <span className="shrink-0 text-xs text-slate-400">{formatBytes(doc.markdown_size_bytes)}</span>
                                </div>
                              </div>
                            </td>
                            <td className="px-4 py-4">
                              <span className={`inline-flex rounded-full px-2 py-1 text-xs font-medium ${documentStatus[doc.status].className}`}>
                                {documentStatus[doc.status].label}
                              </span>
                              <div className="mt-2 text-xs text-slate-400">{doc.chunk_count} 个分块</div>
                              {doc.status === "pending" && doc.review_reason && (
                                <div className="mt-2 text-xs leading-5 text-amber-700" title={doc.review_reason}>
                                  {doc.review_reason}
                                </div>
                              )}
                            </td>
                            <td className="px-4 py-4 text-slate-600">{doc.visibility === "public" ? "公共" : "本课题组"}</td>
                            <td className="truncate px-4 py-4 text-slate-600" title={doc.uploader_username || "未知"}>
                              {doc.uploader_username || "未知"}
                            </td>
                            <td className="px-4 py-4 text-xs leading-5 text-slate-500">{formatDate(doc.created_at)}</td>
                            <td className="px-4 py-4">
                              <div className="flex flex-col items-end gap-2">
                                {doc.status === "pending" && userRole === "admin" && (
                                  <button
                                    className="text-xs font-medium text-emerald-700 hover:text-emerald-900"
                                    onClick={() => void approveDocument(doc)}
                                  >
                                    审核通过
                                  </button>
                                )}
                                <button
                                  className="text-xs font-medium text-slate-700 hover:text-slate-950 disabled:cursor-not-allowed disabled:text-slate-300"
                                  disabled={!doc.has_pdf}
                                  onClick={() => void downloadDocument(doc, "pdf")}
                                >
                                  下载 PDF
                                </button>
                                <button className="text-xs font-medium text-slate-700 hover:text-slate-950" onClick={() => void downloadDocument(doc, "md")}>
                                  下载 MD
                                </button>
                                {doc.can_delete && (
                                  <button
                                    className="text-xs font-medium text-rose-600 hover:text-rose-800 disabled:opacity-50"
                                    disabled={deletingDocument === doc.id}
                                    onClick={() => void deleteDocument(doc)}
                                  >
                                    {deletingDocument === doc.id ? "删除中" : "删除"}
                                  </button>
                                )}
                              </div>
                            </td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>

                  <div className="space-y-3 md:hidden">
                    {documents.map((doc) => (
                      <article key={doc.id} className="rounded-xl border border-slate-200 bg-white p-4 shadow-sm">
                        <div className="flex items-start justify-between gap-3">
                          <div className="min-w-0">
                            <div className="truncate font-medium">{doc.source_filename || doc.filename}</div>
                            <div className="mt-1 truncate text-xs text-slate-500">{doc.filename}</div>
                          </div>
                          <span className={`shrink-0 rounded-full px-2 py-1 text-xs font-medium ${documentStatus[doc.status].className}`}>
                            {documentStatus[doc.status].label}
                          </span>
                        </div>
                        <div className="mt-3 grid grid-cols-2 gap-2 text-xs text-slate-500">
                          <span>{doc.visibility === "public" ? "公共" : "本课题组"}</span>
                          <span>{doc.chunk_count} 个分块</span>
                          <span>{doc.uploader_username || "未知上传者"}</span>
                          <span>{formatDate(doc.created_at)}</span>
                        </div>
                        {doc.status === "pending" && doc.review_reason && (
                          <div className="mt-3 text-xs leading-5 text-amber-700">{doc.review_reason}</div>
                        )}
                        <div className="mt-4 flex flex-wrap gap-2 border-t border-slate-100 pt-3">
                          {doc.status === "pending" && userRole === "admin" && (
                            <button className="rounded-md border border-emerald-200 px-2.5 py-1.5 text-xs text-emerald-700" onClick={() => void approveDocument(doc)}>审核通过</button>
                          )}
                          <button className="rounded-md border border-slate-200 px-2.5 py-1.5 text-xs disabled:text-slate-300" disabled={!doc.has_pdf} onClick={() => void downloadDocument(doc, "pdf")}>下载 PDF</button>
                          <button className="rounded-md border border-slate-200 px-2.5 py-1.5 text-xs" onClick={() => void downloadDocument(doc, "md")}>下载 MD</button>
                          {doc.can_delete && (
                            <button className="rounded-md border border-rose-200 px-2.5 py-1.5 text-xs text-rose-600 disabled:opacity-50" disabled={deletingDocument === doc.id} onClick={() => void deleteDocument(doc)}>
                              {deletingDocument === doc.id ? "删除中" : "删除"}
                            </button>
                          )}
                        </div>
                      </article>
                    ))}
                  </div>
                </>
              )}
            </section>
          )}
        </div>

        {activeView === "chat" && (
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
        )}
      </div>
    </div>
  );
}
