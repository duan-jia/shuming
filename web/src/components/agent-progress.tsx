export interface ToolProgress {
  id: string;
  name: string;
  turn: number;
  status: "running" | "completed" | "failed";
}

export function toolLabel(name: string) {
  const labels: Record<string, string> = {
    display_html: "展示富内容",
    search_knowledge: "知识库检索",
    fetch_knowledge: "读取论文",
    search_web: "网页搜索",
    fetch_web: "读取网页",
  };
  return labels[name] || "工具调用";
}

export function AgentProgress({ turn, tools, streaming }: {
  turn: number;
  tools: ToolProgress[];
  streaming: boolean;
}) {
  if (!turn && !tools.length) return null;
  return (
    <details className="agent-progress reasoning-panel" open={streaming}>
      <summary>工具流程 · {turn ? `第 ${turn} 轮` : "准备中"} · {tools.length} 次调用</summary>
      <ol aria-label="工具调用流程">
        {tools.map(tool => (
          <li key={`${tool.turn}:${tool.id}`} data-status={tool.status}>
            <span>第 {tool.turn} 轮 · {toolLabel(tool.name)}</span>
            <span>{tool.status === "completed" ? "已完成" : tool.status === "failed" ? "未完成" : streaming ? "进行中…" : "已中断"}</span>
          </li>
        ))}
      </ol>
    </details>
  );
}
