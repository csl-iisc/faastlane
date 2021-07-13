import json
import time
import datetime
from util import *

def main(event):
    startTime = 1000*time.time()

    portfolio = event['body']['portfolio']
    portfolios = json.loads(open('data/portfolios.json', 'r').read())
    data = portfolios[portfolio]

    valid = True

    for trade in data:
        trddate = trade['TradeDate']
        # Tag ID: 75, Tag Name: TradeDate, Format: YYMMDD
        if len(trddate) == 6:
            try:
                datetime.datetime(int(trddate[0:2]), int(trddate[2:4]), int(trddate[4:6]))
            except ValueError:
                valid = False
                break
        else:
            valid = False
            break

    response = {'statusCode': 200, 'body': {'valid':valid, 'portfolio': portfolio}}
    endTime = 1000*time.time()
    return timestamp(response, event,startTime, endTime, 0)

# if __name__=="__main__":
#     print(main({"body": {"portfolioType": "S&P","portfolio": "1234"}}))
