import boto3
from util import *

def main(event, context):
    startTime = 1000*time.time()

    dynamodb = boto3.client('dynamodb',aws_access_key_id="AKIAQ4WHHPCKGVH4HO6S",
                       aws_secret_access_key="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",
                       region_name="us-east-1")

    #select correct table based on input data
    if event['body']['reviewType'] == 0:
        tableName = 'faastlane-products-table'
    elif event['body']['reviewType'] == 1:
        tableName = 'faastlane-services-table'
    else:
        raise Exception("Input review is neither Product nor Service")

    #Not publishing to table to avoid network delays in experiments
    response = {'statusCode':200, 'body': event['body']}
    endTime = 1000*time.time()
    return timestamp(response, event, startTime, endTime, 0)
