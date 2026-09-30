-- Rodar no Console do banco D1 (Cloudflare) antes de publicar o Worker

CREATE TABLE IF NOT EXISTS movimentos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL            
);

CREATE TABLE IF NOT EXISTS luz (
  id    INTEGER PRIMARY KEY AUTOINCREMENT,
  valor INTEGER NOT NULL,        
  acesa INTEGER NOT NULL CHECK (acesa IN (0, 1)),
  ts    INTEGER NOT NULL         
);

CREATE INDEX IF NOT EXISTS idx_movimentos_ts ON movimentos (ts);
