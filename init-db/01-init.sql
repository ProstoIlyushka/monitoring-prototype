-- Таблица метрик (связь с агентами будет добавлена позже)
CREATE TABLE IF NOT EXISTS metrics (
    id SERIAL PRIMARY KEY,
    cpu FLOAT NOT NULL,
    memory FLOAT NOT NULL,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Индекс для быстрых выборок последних данных
CREATE INDEX IF NOT EXISTS idx_metrics_timestamp ON metrics(timestamp DESC);

-- Таблица алертов
CREATE TABLE IF NOT EXISTS alerts (
    id SERIAL PRIMARY KEY,
    type VARCHAR(50) NOT NULL,
    message TEXT NOT NULL,
    triggered_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    is_resolved BOOLEAN DEFAULT false,
    resolved_at TIMESTAMP
);

-- Индекс для активных алертов
CREATE INDEX IF NOT EXISTS idx_alerts_active ON alerts(is_resolved, triggered_at DESC);

-- ============================================
-- НОВЫЕ ТАБЛИЦЫ ДЛЯ ПОДДЕРЖКИ НЕСКОЛЬКИХ АГЕНТОВ
-- ============================================

-- Таблица агентов
CREATE TABLE IF NOT EXISTS agents (
    id SERIAL PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    api_key VARCHAR(64) UNIQUE NOT NULL,
    last_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Добавляем agent_id в таблицу metrics
-- Сначала создаем колонку, потом внешний ключ
DO $$ 
BEGIN
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns 
                   WHERE table_name='metrics' AND column_name='agent_id') THEN
        ALTER TABLE metrics ADD COLUMN agent_id INTEGER;
    END IF;
END $$;

-- Добавляем внешний ключ (если его нет)
DO $$ 
BEGIN
    IF NOT EXISTS (SELECT 1 FROM information_schema.table_constraints 
                   WHERE constraint_name='fk_metrics_agent' 
                   AND table_name='metrics') THEN
        ALTER TABLE metrics ADD CONSTRAINT fk_metrics_agent 
            FOREIGN KEY (agent_id) REFERENCES agents(id) ON DELETE SET NULL;
    END IF;
END $$;

-- Индекс для быстрых запросов по агентам
CREATE INDEX IF NOT EXISTS idx_metrics_agent ON metrics(agent_id, timestamp DESC);

-- Индекс для поиска по API-ключам
CREATE INDEX IF NOT EXISTS idx_agents_api_key ON agents(api_key);