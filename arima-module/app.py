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

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8081, threaded=True)
