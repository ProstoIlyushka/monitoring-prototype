-- 01-init.sql
-- Инициализация базы данных для системы мониторинга Linux-серверов

-- ============================================================
-- 1. Таблица агентов (мониторируемые серверы)
-- ============================================================
CREATE TABLE agents (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    api_key VARCHAR(64) NOT NULL UNIQUE,
    status VARCHAR(20) DEFAULT 'active',
    last_seen TIMESTAMP,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Индекс для быстрого поиска по api_key (аутентификация)
CREATE INDEX idx_agents_api_key ON agents(api_key);

-- ============================================================
-- 2. Таблица метрик (нормализованная)
-- ============================================================
CREATE TABLE metrics (
    id BIGSERIAL,
    agent_id INTEGER NOT NULL REFERENCES agents(id) ON DELETE CASCADE,
    metric_name VARCHAR(50) NOT NULL,
    value DOUBLE PRECISION NOT NULL,
    timestamp TIMESTAMP NOT NULL
) PARTITION BY RANGE (timestamp);

-- Составной индекс для быстрых запросов
CREATE INDEX idx_metrics_agent_name_time ON metrics(agent_id, metric_name, timestamp);

-- ============================================================
-- 3. Секционирование (партиции по месяцам)
-- ============================================================

-- Партиции на апрель, май, июнь 2026 года
CREATE TABLE metrics_2026_04 PARTITION OF metrics
    FOR VALUES FROM ('2026-04-01') TO ('2026-05-01');

CREATE TABLE metrics_2026_05 PARTITION OF metrics
    FOR VALUES FROM ('2026-05-01') TO ('2026-06-01');

CREATE TABLE metrics_2026_06 PARTITION OF metrics
    FOR VALUES FROM ('2026-06-01') TO ('2026-07-01');

-- ============================================================
-- 4. Таблица прогнозов
-- ============================================================
CREATE TABLE forecasts (
    id SERIAL PRIMARY KEY,
    agent_id INTEGER NOT NULL REFERENCES agents(id) ON DELETE CASCADE,
    metric_name VARCHAR(50) NOT NULL,
    forecast_value DOUBLE PRECISION NOT NULL,
    forecast_timestamp TIMESTAMP NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Индекс для быстрого получения прогнозов
CREATE INDEX idx_forecasts_agent_metric_time ON forecasts(agent_id, metric_name, forecast_timestamp);

-- ============================================================
-- 5. Таблица оповещений (алёртов) — ИСПРАВЛЕНА
-- ============================================================
CREATE TABLE alerts (
    id SERIAL PRIMARY KEY,
    agent_id INTEGER REFERENCES agents(id) ON DELETE SET NULL,
    metric_name VARCHAR(50) NOT NULL,
    threshold_value DOUBLE PRECISION NOT NULL,
    actual_value DOUBLE PRECISION NOT NULL,
    message TEXT,
    sent_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    email_recipient VARCHAR(100),
    resolved_at TIMESTAMP,           -- Добавлено! Когда оповещение обработано
    is_resolved BOOLEAN DEFAULT FALSE -- Добавлено! Для удобства
);

-- Индекс для истории оповещений
CREATE INDEX idx_alerts_agent_time ON alerts(agent_id, sent_at);
CREATE INDEX idx_alerts_resolved ON alerts(resolved_at) WHERE resolved_at IS NULL;

-- ============================================================
-- 6. Функция автоматического создания партиций (опционально)
-- ============================================================

CREATE OR REPLACE FUNCTION create_next_month_partition()
RETURNS void AS $$
DECLARE
    next_month DATE;
    partition_name TEXT;
    start_date DATE;
    end_date DATE;
BEGIN
    next_month := date_trunc('month', CURRENT_DATE + interval '1 month');
    start_date := next_month;
    end_date := next_month + interval '1 month';
    partition_name := 'metrics_' || to_char(next_month, 'YYYY_MM');
    
    EXECUTE format('
        CREATE TABLE IF NOT EXISTS %I PARTITION OF metrics
        FOR VALUES FROM (%L) TO (%L)',
        partition_name, start_date, end_date
    );
END;
$$ LANGUAGE plpgsql;

-- ============================================================
-- 7. Тестовые данные
-- ============================================================

-- Добавляем тестового агента
INSERT INTO agents (name, api_key, status, last_seen) 
VALUES ('test-server-01', 'test-key-123', 'active', NOW());

-- Несколько тестовых метрик
INSERT INTO metrics (agent_id, metric_name, value, timestamp) VALUES
(1, 'cpu', 45.2, '2026-04-15 10:00:00'),
(1, 'cpu', 47.1, '2026-04-15 10:00:10'),
(1, 'cpu', 48.3, '2026-04-15 10:00:20'),
(1, 'ram', 62.5, '2026-04-15 10:00:00'),
(1, 'ram', 63.1, '2026-04-15 10:00:10'),
(1, 'ram', 61.8, '2026-04-15 10:00:20'),
(1, 'network_rx', 1024000, '2026-04-15 10:00:00'),
(1, 'network_tx', 512000, '2026-04-15 10:00:00');

-- Тестовый прогноз
INSERT INTO forecasts (agent_id, metric_name, forecast_value, forecast_timestamp) VALUES
(1, 'cpu', 50.5, '2026-04-15 11:00:00'),
(1, 'cpu', 52.1, '2026-04-15 11:10:00');

-- Тестовое оповещение (с новыми колонками)
INSERT INTO alerts (agent_id, metric_name, threshold_value, actual_value, message, email_recipient) VALUES
(1, 'cpu', 80.0, 85.3, 'CPU usage exceeded 80%', 'admin@example.com');

-- ============================================================
-- 8. Комментарии для документации
-- ============================================================
COMMENT ON TABLE agents IS 'Мониторируемые серверы (агенты)';
COMMENT ON TABLE metrics IS 'Метрики состояния серверов (нормализованная таблица с секционированием)';
COMMENT ON TABLE forecasts IS 'Прогнозные значения метрик, рассчитанные моделью ARIMA';
COMMENT ON TABLE alerts IS 'История оповещений о превышении пороговых значений';
COMMENT ON FUNCTION create_next_month_partition() IS 'Автоматическое создание партиции на следующий месяц';