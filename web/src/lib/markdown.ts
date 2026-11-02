import remarkGfm from "remark-gfm";
import remarkCjkFriendly from "remark-cjk-friendly";
import remarkMath from "remark-math";
import rehypeKatex from "rehype-katex";

// Parse CJK-adjacent emphasis without rewriting message text or code examples.
export const markdownPlugins = [remarkGfm, remarkCjkFriendly, remarkMath];
export const markdownRehypePlugins = [rehypeKatex];
