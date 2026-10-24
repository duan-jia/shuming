"use client";

import { memo, useState, useCallback, useRef, useEffect, useLayoutEffect } from "react";
import Image from "next/image";
import { createSseParser } from "../lib/sse";
import { sessionTitle } from "../lib/session-title";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";

interface Message {
  id: string;
  role: "user" | "assistant";
  content: string;
  isStreaming: boolean;
  isReasoning?: boolean;
  reasoning?: string;
  toolStatus?: string;
  notice?: string;
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

function SupportCredit({ className = "" }: { className?: string }) {
  return (
    <p className={`support-credit ${className}`}>
      <span>Supported by</span>
      <span className="support-brand">
        <Image src="/shuming-icon.png" alt="" width={18} height={18} className="support-logo" />
        <span>shuming</span>
      </span>
    </p>
  );
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
  processing: { label: "处理中", className: "bg-blue-400/10 text-blue-300" },
  pending: { label: "待审核", className: "bg-amber-400/10 text-amber-300" },
  ready: { label: "可用", className: "bg-emerald-400/10 text-emerald-300" },
  failed: { label: "失败", className: "bg-rose-400/10 text-rose-300" },
};

const API_BASE = "";
const EMPTY_MESSAGES: Message[] = [];
// Keep completed answers out of the Markdown parser while another reply streams.
const MarkdownContent = memo(function MarkdownContent({ content }: { content: string }) {
  return <div className="prose max-w-none prose-slate"><ReactMarkdown remarkPlugins={[remarkGfm]}>{content}</ReactMarkdown></div>;
});

function CopyMessageButton({ content, role }: { content: string; role: Message["role"] }) {
  const [status, setStatus] = useState<"idle" | "copied" | "failed">("idle");

  useEffect(() => {
    if (status === "idle") return;
    const timer = window.setTimeout(() => setStatus("idle"), 2500);
    return () => window.clearTimeout(timer);
  }, [status]);

  async function copy() {
    try {
      await navigator.clipboard.writeText(content);
      setStatus("copied");
    } catch {
      setStatus("failed");
    }
  }

  return (
    <div className="message-actions">
      <button className="copy-message" type="button" onClick={copy}
        aria-label={role === "user" ? "Copy question" : "Copy answer"}>
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
          {status === "copied" ? <path d="m5 12 4 4L19 6" /> : <>
            <rect x="8" y="8" width="12" height="12" rx="2" />
            <path d="M16 8V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h3" />
          </>}
        </svg>
        <span>Copy</span>
      </button>
      <span className={status === "failed" ? "copy-error" : "sr-only"} role="status">
        {status === "copied" ? "Copied to clipboard" : status === "failed" ? "Copy failed. Please select and copy the text." : ""}
      </span>
    </div>
  );
}

export default function Home() {
  const [sessionMessages, setSessionMessages] = useState<Record<string, Message[]>>({});
  const [input, setInput] = useState("");
  const [generating, setGenerating] = useState<Record<string, boolean>>({});
  const [token, setToken] = useState("");
  const [loggedIn, setLoggedIn] = useState(false);
  const [userRole, setUserRole] = useState<"admin" | "user" | "guest">("user");
  const [username, setUsername] = useState("admin");
  const [password, setPassword] = useState("");
  const [loginError, setLoginError] = useState("");
  const [guestEnabled, setGuestEnabled] = useState(false);
  const [loginPending, setLoginPending] = useState(false);
  const [loginCooldown, setLoginCooldown] = useState(0);
  const loginInFlight = useRef(false);
  const [sessions, setSessions] = useState<Session[]>([]);
  const [activeSession, setActiveSession] = useState<string>("");
  const messages = sessionMessages[activeSession] || EMPTY_MESSAGES;
  const loading = !!generating[activeSession];
  const requests = useRef(new Map<string, AbortController>());
  const requestIds = useRef(new Map<string, string>());
  const historyVersions = useRef(new Map<string, number>());
  const navigationVersion = useRef(0);
  const historySequence = useRef(0);
  const activeSessionRef = useRef(activeSession);
  const [sidebarOpen, setSidebarOpen] = useState(false);
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
  const followOutput = useRef(true);
  const [showJumpToLatest, setShowJumpToLatest] = useState(false);
  const messagePaneRef = useRef<HTMLElement>(null);
  const pdfInputRef = useRef<HTMLInputElement>(null);

  useLayoutEffect(() => {
    const pane = messagePaneRef.current;
    // Scroll only the message pane, before paint. scrollIntoView also moves
    // ancestor scroll containers and can pull the composer out of the viewport.
    if (activeView === "chat" && followOutput.current && pane) pane.scrollTop = pane.scrollHeight;
  }, [messages, activeView]);

  useEffect(() => {
    const pending = requests.current;
    return () => {
      for (const controller of pending.values()) controller.abort();
      pending.clear();
    };
  }, []);

  useEffect(() => {
    const controller = new AbortController();
    void fetch("/api/auth/config", { cache: "no-store", signal: controller.signal })
      .then(response => response.ok ? response.json() : null)
      .then(data => setGuestEnabled(data?.guest_enabled === true))
      .catch(() => {});
    return () => controller.abort();
  }, []);

  useEffect(() => {
    if (loginCooldown <= 0) return;
    const timer = window.setTimeout(() => setLoginCooldown(value => Math.max(0, value - 1)), 1000);
    return () => window.clearTimeout(timer);
  }, [loginCooldown]);

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

  const startNewChat = useCallback(() => {
    ++navigationVersion.current;
    // A new chat is a local draft until the first question is submitted.
    // Keep replies in existing sessions running in the background.
    requests.current.get("")?.abort();
    requests.current.delete("");
    setGenerating((prev) => ({ ...prev, "": false }));
    activeSessionRef.current = "";
    setActiveSession("");
    setActiveView("chat");
    setInput("");
    followOutput.current = true;
    setShowJumpToLatest(false);
  }, []);

  const switchSession = useCallback(async (id: string) => {
    ++navigationVersion.current;
    activeSessionRef.current = id;
    setActiveView("chat");
    setActiveSession(id);
    followOutput.current = true;
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
          (m: { id: number; role: string; content: string; reasoning?: string }) => ({
            id: String(m.id),
            role: m.role as "user" | "assistant",
            content: m.content || "",
            reasoning: m.reasoning || "",
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

  const login = useCallback(async (mode: "password" | "guest" = "password") => {
    if (loginInFlight.current || loginCooldown > 0) return;
    loginInFlight.current = true;
    setLoginPending(true);
    setLoginError("");
    try {
      const res = await fetch(`${API_BASE}/api/auth/${mode === "guest" ? "guest" : "login"}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(mode === "guest" ? {} : { username, password }),
      });
      const data = await res.json();
      if (res.ok && typeof data.token === "string" && data.token) {
        setToken(data.token);
        setUserRole(data.user?.role === "admin" ? "admin" : data.user?.role === "guest" ? "guest" : "user");
        setActiveView("chat");
        setPassword("");
        setLoggedIn(true);
      } else if (res.status === 429) {
        const delay = Number(res.headers.get("Retry-After"));
        setLoginCooldown(Number.isFinite(delay) && delay > 0 ? Math.min(Math.ceil(delay), 3600) : 60);
        setLoginError("尝试过于频繁，请稍后再试。");
      } else {
        setLoginError(res.status === 401 ? "用户名或密码不正确，或账号暂不可用。" : mode === "guest" ? "游客体验暂不可用，请使用账号登录。" : "暂时无法登录，请稍后再试。");
      }
    } catch {
      setLoginError("暂时无法登录，请稍后再试。");
    } finally {
      loginInFlight.current = false;
      setLoginPending(false);
    }
  }, [username, password, loginCooldown]);

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
          body: JSON.stringify({ title: sessionTitle(input) }),
          signal: controller.signal,
        });
        const data = await res.json();
        if (requests.current.get("") !== controller) return;
        if (!res.ok || !data.id) throw new Error(res.status === 429 ? "体验次数已达上限，请稍后再试或登录账号。" : res.status === 401 ? "登录已过期，请退出后重新登录。" : "无法创建会话，请稍后重试。");
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
      } catch (error) {
        if (!controller.signal.aborted) setUploadNotice({ type: "error", message: error instanceof Error ? error.message : "无法创建会话。" });
        if (requests.current.get("") === controller) {
          requests.current.delete("");
          setGenerating((prev) => ({ ...prev, "": false }));
        }
        return;
      }
    }

    historyVersions.current.set(sid, ++historySequence.current);
    let pendingUpdates: ((messages: Message[]) => Message[])[] = [];
    let updateTimer: ReturnType<typeof setTimeout> | undefined;
    const flushMessages = () => {
      clearTimeout(updateTimer);
      updateTimer = undefined;
      const updates = pendingUpdates;
      pendingUpdates = [];
      if (!updates.length || requests.current.get(sid) !== controller) return;
      setSessionMessages((prev) => ({ ...prev, [sid]: updates.reduce((value, update) => update(value), prev[sid] || []) }));
    };
    const setMessages = (update: (messages: Message[]) => Message[]) => {
      if (requests.current.get(sid) !== controller) return;
      pendingUpdates.push(update);
      // Network chunks can arrive much faster than the UI can parse Markdown.
      if (updateTimer === undefined) updateTimer = setTimeout(flushMessages, 50);
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

    followOutput.current = true;
    setShowJumpToLatest(false);
    const requestId = createRequestId();
    requestIds.current.set(sid, requestId);
    try {
      const res = await fetch(`${API_BASE}/api/chat/stream`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${token}`,
          "X-Request-Id": requestId,
        },
        body: JSON.stringify({ message: userMsg.content, session_id: sid }),
        signal: controller.signal,
      });

      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const reader = res.body?.getReader();
      if (!reader) throw new Error("No reader");

      const decoder = new TextDecoder();
      let finished = false;
      const parse = createSseParser(({ event, data }) => {
        if (finished) return;
        if (data === "[DONE]") { finished = true; return; }
        const parsed = JSON.parse(data);
        if (event === "error" || parsed.type === "error" || parsed.error) {
          finished = true;
          setMessages((prev) =>
            prev.map((message) =>
              message.id === msgId
                ? {
                    ...message,
                    content: message.content,
                    notice: parsed.stop_reason === "cancelled" ? "已停止生成" : parsed.stop_reason === "output_limit" ? "本轮推理达到长度上限，请缩短问题或联系管理员调整输出上限。" : parsed.error || "生成失败",
                    toolStatus: undefined,
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
                    toolStatus: undefined,
                    isReasoning: false,
                  }
                : message
            )
          );
        } else if (parsed.type === "tool_start" || parsed.type === "tool_end") {
          const toolNames: Record<string, string> = {
            search_knowledge: "知识库检索",
            fetch_knowledge: "读取论文",
            search_web: "网页搜索",
            fetch_web: "读取网页",
          };
          const name = toolNames[parsed.name] || "工具调用";
          setMessages((prev) => prev.map((message) => message.id === msgId
            ? { ...message, isReasoning: false, toolStatus: parsed.type === "tool_start" ? `正在${name}…` : parsed.is_error ? `${name}未完成，正在继续处理…` : `${name}完成，正在整理…` }
            : message));
        } else if (parsed.type === "reasoning") {
          setMessages((prev) =>
            prev.map((message) =>
              message.id === msgId ? { ...message, isReasoning: true, reasoning: (message.reasoning || "") + (parsed.content || "") } : message
            )
          );
        } else if (parsed.type === "done") {
          finished = true;
          setMessages((prev) =>
            prev.map((message) =>
              message.id === msgId
                ? { ...message, isStreaming: false, isReasoning: false, toolStatus: undefined, notice: parsed.truncated ? "回答已达到长度上限，可发送“继续”接着生成。" : message.notice }
                : message
            )
          );
        }
      });
      try {
        while (!finished) {
          const { done, value } = await reader.read();
          if (done) { parse(decoder.decode()); break; }
          parse(decoder.decode(value, { stream: true }));
        }
        if (!finished) throw new Error("连接中断，请重试");
      } finally {
        await reader.cancel().catch(() => {});
        reader.releaseLock();
      }
    } catch (error) {
      if (controller.signal.aborted) {
        setMessages((prev) => prev.map((message) => message.id === msgId ? { ...message, notice: "已停止生成" } : message));
        return;
      }
      const errorMessage =
        error instanceof Error && error.message
          ? `请求失败：${error.message}`
          : "暂时无法生成答案，请稍后再试。";
      setMessages((prev) =>
        prev.map((message) =>
          message.id === msgId
            ? {
                ...message,
                notice: errorMessage,
                isStreaming: false,
              }
            : message
        )
      );
    } finally {
      setMessages((prev) => prev.map((message) =>
        message.id === msgId ? { ...message, isStreaming: false, isReasoning: false, toolStatus: undefined } : message
      ));
      // Publish the tail and terminal state before releasing this request.
      flushMessages();
      if (requests.current.get(sid) === controller) {
        requests.current.delete(sid);
        requestIds.current.delete(sid);
        setGenerating((prev) => ({ ...prev, [sid]: false }));
      }
    }
  }, [input, token, activeSession, fetchSessions]);

  const stopGeneration = useCallback(async () => {
    const controller = requests.current.get(activeSession);
    const requestId = requestIds.current.get(activeSession);
    if (!controller) return;
    controller.abort();
    if (requestId) {
      await fetch(`${API_BASE}/api/chat/cancel`, {
        method: "POST",
        headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}` },
        body: JSON.stringify({ session_id: activeSession, request_id: requestId }),
      }).catch(() => {});
    }
  }, [activeSession, token]);

  if (!loggedIn) {
    return (
      <main className="login-page">
        <section className="login-panel" aria-label="登录 EMOS">
          <h1 className="wordmark login-wordmark">EMOS</h1>
          <form onSubmit={(event) => { event.preventDefault(); void login(); }}>
            {loginError && <div role="alert" className="mb-5 rounded-lg bg-amber-400/10 px-3 py-2 text-sm text-amber-300">{loginError}</div>}
            <label className="login-label">
              用户名
              <input className="login-input" autoComplete="username" value={username} onChange={(e) => setUsername(e.target.value)} required />
            </label>
            <label className="login-label">
              密码
              <input className="login-input" type="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} required />
            </label>
            <button className="primary-button mt-8 w-full disabled:opacity-50" type="submit" disabled={loginPending || loginCooldown > 0}>{loginPending ? "正在登录…" : loginCooldown > 0 ? `请等待 ${loginCooldown} 秒` : "登录"}</button>
          </form>
          {guestEnabled && <div className="guest-entry">
            <button type="button" className="guest-button" disabled={loginPending || loginCooldown > 0} onClick={() => void login("guest")}>游客体验</button>
            <p>仅公开知识 · 15 分钟体验 · 无上传权限</p>
          </div>}
          <SupportCredit className="login-support" />
        </section>
      </main>
    );
  }

  return (
    <div className="flex h-dvh overflow-hidden bg-background text-foreground">
      {sidebarOpen && <button aria-label="关闭导航" className="fixed inset-0 z-20 bg-black/60 backdrop-blur-sm md:hidden" onClick={() => setSidebarOpen(false)} />}
      <aside className={`sidebar fixed inset-y-0 left-0 z-30 flex min-h-0 w-64 shrink-0 flex-col border-r border-line bg-sidebar text-foreground transition-transform md:static md:translate-x-0 ${sidebarOpen ? "translate-x-0" : "-translate-x-full"}`}>
        <button className="absolute right-3 top-3 rounded-lg p-2 text-neutral-400 md:hidden" aria-label="关闭导航" onClick={() => setSidebarOpen(false)}>×</button>
        <div className="shrink-0 border-b border-line p-5">
          <div className="wordmark sidebar-wordmark">EMOS</div>
          <button
            className="mt-7 w-full rounded-lg border border-line px-3 py-2.5 text-sm text-neutral-300 transition hover:bg-white/5"
            onClick={() => { startNewChat(); setSidebarOpen(false); }}
          >
            新建对话
          </button>
          <div className={`mt-3 grid gap-2 ${userRole === "guest" ? "grid-cols-1" : "grid-cols-2"}`}>
            <button
              className={`rounded-lg px-3 py-2 text-sm transition ${
                activeView === "chat" ? "bg-white/10 text-foreground" : "text-neutral-500 hover:bg-white/5 hover:text-foreground"
              }`}
              onClick={() => {
                setActiveView("chat");
                setSidebarOpen(false);
                setUploadNotice(null);
              }}
            >
              对话
            </button>
            {userRole !== "guest" && (            <button
              className={`rounded-lg px-3 py-2 text-sm transition ${
                activeView === "documents" ? "bg-white/10 text-foreground" : "text-neutral-500 hover:bg-white/5 hover:text-foreground"
              }`}
              onClick={() => {
                setActiveView("documents");
                setSidebarOpen(false);
                setUploadNotice(null);
                void fetchDocuments(true);
              }}
            >
              文档管理
            </button>)}
          </div>
        </div>
        <div className="min-h-0 flex-1 overflow-y-auto overscroll-y-contain p-3">
          {activeView === "chat" ? (
            <>
              {sessions.map((s) => (
                <div
                  key={s.id}
                  className={`group flex cursor-pointer items-center rounded-md px-3 py-2 hover:bg-white/5 ${
                    activeSession === s.id ? "bg-white/10" : ""
                  }`}
                  onClick={() => { void switchSession(s.id); setSidebarOpen(false); }}
                >
                  <span className="min-w-0 flex-1 truncate text-sm" title={s.title}>{s.title}</span>
                  <button
                    aria-label={`删除会话 ${s.title}`} className="text-xs text-neutral-500 hover:text-red-300 md:opacity-0 md:group-hover:opacity-100 md:focus:opacity-100"
                    onClick={(e) => deleteSession(s.id, e)}
                  >
                    x
                  </button>
                </div>
              ))}
            </>
          ) : null}
        </div>
        <div className="shrink-0 border-t border-line p-4">
          {userRole === "guest" && <p className="mb-3 text-xs text-neutral-400">游客体验 · 仅公开知识</p>}
          <button
            className="text-sm text-neutral-500 transition hover:text-foreground"
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
          <SupportCredit className="sidebar-support" />
        </div>
      </aside>

      <div className="flex min-h-0 min-w-0 flex-1 flex-col">
        <header className={`flex shrink-0 items-center justify-between bg-background px-5 py-4 ${activeView === "chat" ? "chat-navigation md:hidden" : "chat-header"}`}>
          <div className="flex items-center gap-3">
            <button className="rounded-lg border border-line px-2.5 py-1.5 text-neutral-300 md:hidden" aria-label="打开导航" aria-expanded={sidebarOpen} onClick={() => setSidebarOpen(true)}><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" aria-hidden="true"><path d="M4 6h16M4 12h16M4 18h16" /></svg></button>
            {activeView === "documents" && <h1 className="text-sm font-medium">文档管理</h1>}
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
                className="rounded-lg bg-foreground px-4 py-2 text-sm font-medium text-background transition hover:bg-white disabled:cursor-not-allowed disabled:opacity-50"
                type="button"
                onClick={() => pdfInputRef.current?.click()}
                disabled={uploading}
              >
                {uploading ? "上传并处理中" : "上传 PDF"}
              </button>
            </div>
          )}
        </header>

        <main ref={messagePaneRef} onScroll={(event) => { if (activeView !== "chat") return; const el = event.currentTarget; followOutput.current = el.scrollHeight - el.scrollTop - el.clientHeight < 100; setShowJumpToLatest(!followOutput.current); }} className={`message-pane relative mx-auto min-h-0 w-full flex-1 overflow-y-auto overscroll-y-contain px-5 py-5 ${activeView === "chat" ? "chat-content" : "max-w-7xl"}`}>
          {activeView === "chat" && <h1 className="sr-only">知识对话</h1>}
          {uploadNotice && (
            <div
              className={`mb-5 rounded-lg border px-4 py-3 text-sm ${
                uploadNotice.type === "success"
                  ? "border-emerald-400/20 bg-emerald-400/10 text-emerald-300"
                  : "border-amber-400/20 bg-amber-400/10 text-amber-300"
              }`}
              role="status"
            >
              {uploadNotice.message}
            </div>
          )}
          {activeView === "chat" ? (
            <>
              {messages.length === 0 && (
                <div className="chat-welcome">
                  <div className="welcome-content">
                    <h2 className="wordmark">EMOS</h2>
                    <p>What would you like to explore?</p>
                    <div className="welcome-prompts" aria-label="Suggested questions">
                      {[
                        "What makes a good research question?",
                        "How do I compare two approaches?",
                        "How can I turn an idea into a plan?",
                      ].map((question) => (
                        <button key={question} type="button" onClick={() => {
                          setInput(question);
                          document.querySelector<HTMLTextAreaElement>('textarea[aria-label="输入问题"]')?.focus();
                        }}>{question}</button>
                      ))}
                    </div>
                  </div>
                </div>
              )}
              {messages.map((msg) => (
                <article key={msg.id} className={`message-row ${msg.role}`} aria-label={msg.role === "user" ? "你的问题" : "EMOS 的回答"}>
                  <div className="message-body" aria-busy={msg.isStreaming}>
                    {msg.role === "assistant" ? <>
                      {msg.reasoning && <details className="reasoning-panel"><summary>{msg.isReasoning ? "正在思考…" : "思考过程"}</summary><div>{msg.reasoning}</div></details>}
                      <MarkdownContent content={msg.content} />
                      {msg.isStreaming && <div className="generation-status" role="status"><span className="stream-dot" aria-hidden="true" /><span className="sr-only">{msg.toolStatus || (msg.isReasoning ? "正在思考" : "正在生成回答")}</span></div>}
                      {msg.notice && <p role="status" className="message-notice">{msg.notice}</p>}
                    </> : <p className="user-content">{msg.content}</p>}
                    {!msg.isStreaming && msg.content && <CopyMessageButton content={msg.content} role={msg.role} />}
                  </div>
                </article>
              ))}
            </>
          ) : (
            <section>
              <div className="mb-4 flex items-center justify-between">
                <p className="text-sm text-neutral-400">共 {documents.length} 条可见文档</p>
                <button
                  className="text-sm font-medium text-neutral-400 transition hover:text-foreground disabled:opacity-50"
                  onClick={() => void fetchDocuments(true)}
                  disabled={documentsLoading}
                >
                  {documentsLoading ? "刷新中" : "刷新"}
                </button>
              </div>

              {documentsError && (
                <div className="mb-4 rounded-lg border border-amber-400/20 bg-amber-400/10 px-4 py-3 text-sm text-amber-300">
                  {documentsError}
                </div>
              )}

              {documentsLoading && documents.length === 0 ? (
                <div className="py-20 text-center text-sm text-neutral-500">
                  正在加载文档…
                </div>
              ) : documents.length === 0 ? (
                <div className="py-20 text-center text-sm text-neutral-500">暂无文档</div>
              ) : (
                <>
                  <div className="hidden overflow-hidden rounded-xl border border-line bg-surface md:block">
                    <table className="w-full table-fixed text-left text-sm">
                      <thead className="border-b border-line bg-white/5 text-xs font-medium uppercase tracking-wide text-neutral-400">
                        <tr>
                          <th className="w-[34%] px-4 py-3">文件</th>
                          <th className="w-[12%] px-4 py-3">状态</th>
                          <th className="w-[13%] px-4 py-3">范围</th>
                          <th className="w-[14%] px-4 py-3">上传者</th>
                          <th className="w-[15%] px-4 py-3">创建时间</th>
                          <th className="w-[12%] px-4 py-3 text-right">操作</th>
                        </tr>
                      </thead>
                      <tbody className="divide-y divide-line">
                        {documents.map((doc) => (
                          <tr key={doc.id} className="align-top hover:bg-white/5">
                            <td className="px-4 py-4">
                              <div className="min-w-0">
                                <div className="flex items-center gap-2">
                                  <span className="rounded bg-rose-400/10 px-1.5 py-0.5 text-[10px] font-semibold text-rose-300">PDF</span>
                                  <span className="truncate font-medium" title={doc.source_filename || "无源 PDF"}>
                                    {doc.source_filename || "历史记录无源 PDF"}
                                  </span>
                                  <span className="shrink-0 text-xs text-neutral-500">{formatBytes(doc.source_size_bytes)}</span>
                                </div>
                                <div className="mt-2 flex items-center gap-2 text-neutral-400">
                                  <span className="rounded bg-white/10 px-1.5 py-0.5 text-[10px] font-semibold text-neutral-400">MD</span>
                                  <span className="truncate" title={doc.filename}>{doc.filename}</span>
                                  <span className="shrink-0 text-xs text-neutral-500">{formatBytes(doc.markdown_size_bytes)}</span>
                                </div>
                              </div>
                            </td>
                            <td className="px-4 py-4">
                              <span className={`inline-flex rounded-full px-2 py-1 text-xs font-medium ${documentStatus[doc.status].className}`}>
                                {documentStatus[doc.status].label}
                              </span>
                              <div className="mt-2 text-xs text-neutral-500">{doc.chunk_count} 个分块</div>
                              {doc.status === "pending" && doc.review_reason && (
                                <div className="mt-2 text-xs leading-5 text-amber-300" title={doc.review_reason}>
                                  {doc.review_reason}
                                </div>
                              )}
                            </td>
                            <td className="px-4 py-4 text-neutral-400">{doc.visibility === "public" ? "公共" : "本课题组"}</td>
                            <td className="truncate px-4 py-4 text-neutral-400" title={doc.uploader_username || "未知"}>
                              {doc.uploader_username || "未知"}
                            </td>
                            <td className="px-4 py-4 text-xs leading-5 text-neutral-400">{formatDate(doc.created_at)}</td>
                            <td className="px-4 py-4">
                              <div className="flex flex-col items-end gap-2">
                                {doc.status === "pending" && userRole === "admin" && (
                                  <button
                                    className="text-xs font-medium text-emerald-300 hover:text-emerald-900"
                                    onClick={() => void approveDocument(doc)}
                                  >
                                    审核通过
                                  </button>
                                )}
                                <button
                                  className="text-xs font-medium text-neutral-300 hover:text-foreground disabled:cursor-not-allowed disabled:text-neutral-600"
                                  disabled={!doc.has_pdf}
                                  onClick={() => void downloadDocument(doc, "pdf")}
                                >
                                  下载 PDF
                                </button>
                                <button className="text-xs font-medium text-neutral-300 hover:text-foreground" onClick={() => void downloadDocument(doc, "md")}>
                                  下载 MD
                                </button>
                                {doc.can_delete && (
                                  <button
                                    className="text-xs font-medium text-rose-300 hover:text-rose-200 disabled:opacity-50"
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
                      <article key={doc.id} className="rounded-xl border border-line bg-surface p-4">
                        <div className="flex items-start justify-between gap-3">
                          <div className="min-w-0">
                            <div className="truncate font-medium">{doc.source_filename || doc.filename}</div>
                            <div className="mt-1 truncate text-xs text-neutral-400">{doc.filename}</div>
                          </div>
                          <span className={`shrink-0 rounded-full px-2 py-1 text-xs font-medium ${documentStatus[doc.status].className}`}>
                            {documentStatus[doc.status].label}
                          </span>
                        </div>
                        <div className="mt-3 grid grid-cols-2 gap-2 text-xs text-neutral-400">
                          <span>{doc.visibility === "public" ? "公共" : "本课题组"}</span>
                          <span>{doc.chunk_count} 个分块</span>
                          <span>{doc.uploader_username || "未知上传者"}</span>
                          <span>{formatDate(doc.created_at)}</span>
                        </div>
                        {doc.status === "pending" && doc.review_reason && (
                          <div className="mt-3 text-xs leading-5 text-amber-300">{doc.review_reason}</div>
                        )}
                        <div className="mt-4 flex flex-wrap gap-2 border-t border-line pt-3">
                          {doc.status === "pending" && userRole === "admin" && (
                            <button className="rounded-md border border-emerald-400/20 px-2.5 py-1.5 text-xs text-emerald-300" onClick={() => void approveDocument(doc)}>审核通过</button>
                          )}
                          <button className="rounded-md border border-line px-2.5 py-1.5 text-xs disabled:text-neutral-600" disabled={!doc.has_pdf} onClick={() => void downloadDocument(doc, "pdf")}>下载 PDF</button>
                          <button className="rounded-md border border-line px-2.5 py-1.5 text-xs" onClick={() => void downloadDocument(doc, "md")}>下载 MD</button>
                          {doc.can_delete && (
                            <button className="rounded-md border border-rose-400/20 px-2.5 py-1.5 text-xs text-rose-300 disabled:opacity-50" disabled={deletingDocument === doc.id} onClick={() => void deleteDocument(doc)}>
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
        </main>

        {activeView === "chat" && (
          <div className="composer-area shrink-0 px-4 pb-6 pt-3">
            {showJumpToLatest && messages.length > 0 && <button className="jump-to-latest" onClick={() => { followOutput.current = true; const pane = messagePaneRef.current; if (pane) pane.scrollTop = pane.scrollHeight; setShowJumpToLatest(false); }}>查看最新回答 ↓</button>}
            <div className="composer mx-auto flex gap-2">
              <textarea
                rows={2}
                aria-label="输入问题" className="min-w-0 flex-1 bg-transparent px-3 py-2.5 outline-none"
                placeholder="Ask anything…"
                value={input}
                onChange={(e) => setInput(e.target.value)}
                onKeyDown={(e) => { if (e.key === "Enter" && !e.shiftKey && !e.nativeEvent.isComposing) { e.preventDefault(); void sendMessage(); } }}
                disabled={loading}
              />
              <button
                className="rounded-lg bg-foreground px-5 py-2.5 font-medium text-background transition hover:bg-white disabled:opacity-50"
                onClick={loading ? stopGeneration : sendMessage}
                disabled={!loading && !input.trim()}
              >
                {loading ? "停止生成" : "发送"}
              </button>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
