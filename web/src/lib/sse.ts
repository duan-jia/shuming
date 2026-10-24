/** Incremental SSE decoder: UTF-8 decoding belongs to the caller. */
export interface SseEvent { event: string; data: string }
export function createSseParser(onEvent: (event: SseEvent) => void): (chunk: string) => void {
  let buffer = "";
  let event = "message";
  let data: string[] = [];
  return (chunk: string): void => {
    buffer += chunk;
    let end: number;
    while ((end = buffer.indexOf("\n")) !== -1) {
      const line = buffer.slice(0, end).replace(/\r$/, "");
      buffer = buffer.slice(end + 1);
      if (!line) {
        if (data.length) onEvent({ event, data: data.join("\n") });
        event = "message";
        data = [];
      } else if (!line.startsWith(":")) {
        const colon = line.indexOf(":");
        const field = colon < 0 ? line : line.slice(0, colon);
        const value = colon < 0 ? "" : line.slice(colon + 1).replace(/^ /, "");
        if (field === "event") event = value || "message";
        if (field === "data") data.push(value);
      }
    }
  };
}
