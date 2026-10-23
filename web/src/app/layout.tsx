import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "EMOS",
  description: "EMOS 智能助手",
  icons: {
    icon: "data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22/%3E",
  },
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="zh-CN" className="h-full overflow-hidden antialiased">
      <body className="flex h-full min-h-0 flex-col overflow-hidden">{children}</body>
    </html>
  );
}
