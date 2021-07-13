import json
import time
from util import *

def main(event):
    startTime = 1000*time.time()

    portfolio = event['body']['portfolio']
    portfolios = json.loads(open('data/portfolios.json', 'r').read())
    data = portfolios[portfolio]

    valid = True

    for trade in data:
        px = str(trade['LastPx'])
        if '.' in px:
            a,b = px.split('.')
            if not ((len(a) == 3 and len(b) == 6) or
                    (len(a) == 4 and len(b) == 5) or
                    (len(a) == 5 and len(b) == 4) or
                    (len(a) == 6 and len(b) == 3)):
                print('{}: {}v{}'.format(px, len(a), len(b)))
                valid = False
                break

    response = {'statusCode': 200, 'body': {'valid':valid, 'portfolio': portfolio}}
    endTime = 1000*time.time()
    return timestamp(response, event,startTime, endTime, 0)

if __name__=="__main__":
    print(main({"body": {"portfolioType": "S&P","portfolio": "1234"}}))
