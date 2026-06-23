from flask import Flask, request, jsonify
import numpy as np
from statsmodels.tsa.arima.model import ARIMA

app = Flask(__name__)

@app.route('/forecast', methods=['POST'])
def forecast():
    data = request.json
    history = data.get('history', [])
    horizon = data.get('horizon', 12)
    
    if len(history) < 10:
        return jsonify({'error': 'Need at least 10 historical points'}), 400
    
    if horizon < 1 or horizon > 48:
        return jsonify({'error': 'Horizon must be between 1 and 48'}), 400
    
    try:
        # Простая ARIMA(1,1,1) — стабильная для серверных метрик
        model = ARIMA(history, order=(1, 1, 1))
        fitted = model.fit()
        forecast = fitted.forecast(steps=horizon)
        
        return jsonify({
            'forecast': forecast.tolist(),
            'horizon': horizon,
            'model': 'ARIMA(1,1,1)',
            'aic': fitted.aic
        })
    except Exception as e:
        return jsonify({'error': str(e)}), 500

@app.route('/batch_forecast', methods=['POST'])
def batch_forecast():
    """Принимает: {"agent_id": 2, "metrics": ["cpu", "memory", ...], "horizon": 12}
       Возвращает: {"forecasts": {"cpu": [values], "memory": [values]}}"""
    data = request.json
    agent_id = data.get('agent_id')
    metrics = data.get('metrics', [])
    horizon = data.get('horizon', 12)
    
    result = {}
    for metric in metrics:
        # Здесь нужно получить историю для каждой метрики
        # В реальности данные будут переданы из Drogon
        history = data.get('history', {}).get(metric, [])
        if len(history) >= 10:
            model = ARIMA(history, order=(1, 1, 1))
            fitted = model.fit()
            forecast = fitted.forecast(steps=horizon)
            result[metric] = forecast.tolist()
    
    return jsonify({'agent_id': agent_id, 'forecasts': result, 'horizon': horizon})

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8081, threaded=True)
