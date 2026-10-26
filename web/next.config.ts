import type { NextConfig } from "next";

const backendBase = (process.env.BACKEND_API_BASE || "http://127.0.0.1:9006").replace(/\/$/, "");

const nextConfig: NextConfig = {
  output: "standalone",
  async rewrites() {
    return {
      // The dynamic documents/[id] route otherwise handles these actions and
      // returns 405 for POST before the fallback proxy can run.
      beforeFiles: [
        {
          source: "/api/documents/search",
          destination: `${backendBase}/api/documents/search`,
        },
        {
          source: "/api/documents/deduplicate",
          destination: `${backendBase}/api/documents/deduplicate`,
        },
      ],
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
