import type { NextConfig } from "next";

const backendBase = (process.env.BACKEND_API_BASE || "http://127.0.0.1:9006").replace(/\/$/, "");

const nextConfig: NextConfig = {
  output: "standalone",
  async rewrites() {
    return {
      fallback: [
        {
          source: "/api/:path*",
          destination: `${backendBase}/api/:path*`,
        },
      ],
    };
  },
};

export default nextConfig;
