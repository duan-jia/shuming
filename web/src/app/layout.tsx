import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "EMOS",
  description: "Lab-private LLM and RAG assistant",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="zh-CN" className="h-full antialiased">
      <body className="min-h-full flex flex-col">{children}</body>
    </html>
  );
}
