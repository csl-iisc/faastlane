try:
  import unzip_requirements
except ImportError:
  pass

import time
import nltk
nltk.data.path.append('nltk_data/')
from nltk.sentiment.vader import SentimentIntensityAnalyzer
from util import *

def main(event, context):
    startTime = 1000*time.time()
    sid = SentimentIntensityAnalyzer()
    feedback = event['body']['feedback']
    scores = sid.polarity_scores(feedback)

    if scores['compound'] > 0:
        sentiment = 1
    elif scores['compound'] == 0:
        sentiment = 0
    else:
        sentiment = -1

    #pass through values
    response = {'statusCode' : 200,
                'body' : { 'sentiment': sentiment,
                'reviewType': event['body']['reviewType'] + 0,
                'reviewID': (event['body']['reviewID'] + '0')[:-1],
                'customerID': (event['body']['customerID'] + '0')[:-1],
                'productID': (event['body']['productID'] + '0')[:-1],
                'feedback': (event['body']['feedback'] + '0')[:-1]}}

    endTime = 1000*time.time()
    return timestamp(response, event, startTime, endTime, 0)
