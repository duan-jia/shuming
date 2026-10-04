{
  "server": {
    "port": 9006,
    "static_dir": ""
  },
  "mysql": {
    "host": "mysql",
    "port": 3306,
    "user": "root",
    "password": "__MYSQL_ROOT_PASSWORD__",
    "database": "llm_platform",
    "pool_size": 2
  },
  "jwt": {
    "secret": "__JWT_SECRET__",
    "expiry_seconds": 3600
  },
  "admin": {
    "username": "admin",
    "default_password": "__ADMIN_PASSWORD__"
  },
  "default_tenant": {
    "id": 1,
    "name": "default",
    "daily_token_quota": 100000
  },
  "inference": {
    "n_ctx": 1024,
    "n_batch": 128,
    "n_threads": 2,
    "n_gpu_layers": 0,
    "max_tokens": 256,
    "temperature": 0.7,
    "top_p": 0.9,
    "top_k": 40,
    "repeat_penalty": 1.1,
    "repeat_last_n": 64,
    "system_prompt": "Staging RAG verification service."
  },
  "rag": {
    "storage_dir": "/data/rag",
    "query_rewrite": {
      "enabled": false,
      "model": "disabled",
      "timeout_seconds": 5
    }
  },
  "rate_limiter": {
    "rules": [
      { "action": "chat", "max_requests": 20, "window_seconds": 60 }
    ]
  },
  "scheduler": {
    "num_workers": 1
  },
  "http_threads": 1,
  "remote_engine": {
    "endpoint": "https://api.deepseek.com/v1",
    "model": "deepseek-chat",
    "context_window_tokens": 32768,
    "api_key": ""
  }
}
