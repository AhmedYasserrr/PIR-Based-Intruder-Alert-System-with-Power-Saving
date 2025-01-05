from flask import Flask, render_template, jsonify

app = Flask(__name__)

log_file_path = "logs.txt"

def read_logs_from_file():
    try:
        with open(log_file_path, "r") as f:
            return f.readlines()
    except FileNotFoundError:
        return []

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/logs')
def get_logs():
    logs = read_logs_from_file()
    return jsonify([log.strip() for log in logs])

if __name__ == '__main__':
    app.run(debug=True, host='0.0.0.0')
