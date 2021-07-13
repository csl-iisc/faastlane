import yfinance as yf
import time
from util import *

def main(event):
    startTime = 1000*time.time()
    externalServicesTime = 0
    portfolioType = event['body']['portfolioType']

    tickersForPortfolioTypes = {'S&P': ['GOOG', 'AMZN', 'MSFT']}
    tickers = tickersForPortfolioTypes[portfolioType]

    prices = {}
    for ticker in tickers:
        tickerObj = yf.Ticker(ticker)
        #Get last closing price
        tickTime = 1000*time.time()
        data = tickerObj.history(period="1")
        externalServicesTime += 1000*time.time() - tickTime
        price = data['Close'].unique()[0]
        prices[ticker] = price

    # prices = {'GOOG': 1732.38, 'AMZN': 3185.27, 'MSFT': 221.02}

    response = {'statusCode':200,
            'body': {'marketData':prices}}

    endTime = 1000*time.time()
    return timestamp(response, event, startTime, endTime, externalServicesTime)

# if __name__=="__main__":
#     event = {'body':{'portfolioType':'S&P'}}
#     print(main(event))
