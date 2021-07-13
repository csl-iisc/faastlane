# import boto3
import util

def main(event, context):
    '''
    Sends notification of negative results from sentiment analysis via SNS
    '''

    ##construct message from input data and publish via SNS
    #sns = boto3.client('sns')
    #sns.publish(
    #    TopicArn = 'arn:aws:sns:XXXXXXXXXXXXXXXX:my-SNS-topic',
    #    Subject = 'Negative Review Received',
    #    Message = 'Review (ID = %i) of %s (ID = %i) received with negative results from sentiment analysis. Feedback from Customer (ID = %i): "%s"' % (int(event['body']['reviewID']),
    #                event['body']['reviewType'], int(event['body']['productID']), int(event['body']['customerID']), event['body']['feedback'])
    #)

    #pass through values
    return event
