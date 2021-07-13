from botocore.vendored import requests
import json
import boto3
import logging
import threading

client = boto3.client(service_name='comprehendmedical',aws_access_key_id="AKIA3FFYAI3OUV5UWTKJ",
                       aws_secret_access_key="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", region_name="us-east-1")

def extract_entities_from_message(message):
    return client.detect_phi(Text=message)

def handler(event, context):
    print ('Received message payload. Will extract PII')
    try:
        # Extract the message from the event
        message = event['body']['message']
        # Extract all entities from the message
        entities_response = extract_entities_from_message(message)
        entity_list = entities_response['Entities']
        event['body']['entities'] = entity_list
        print ('PII entity extraction completed')
        # return entity_list
        return event
    except Exception as e:
        logging.error('Exception: %s. Unable to extract PII entities from message' % e)
        raise e

if __name__=="__main__":
    print(handler(
{
    "body":{
    "message": "Pt is 87 yo woman, highschool teacher with past medical history that includes   - status post cardiac catheterization in April 2019.She presents today with palpitations and chest pressure.HPI : Sleeping trouble on present dosage of Clonidine. Severe Rash  on face and leg, slightly itchy  Meds : Vyvanse 50 mgs po at breakfast daily,             Clonidine 0.2 mgs -- 1 and 1 / 2 tabs po qhs HEENT : Boggy inferior turbinates, No oropharyngeal lesion Lungs : clear Heart : Regular rhythm Skin :  Mild erythematous eruption to hairline Follow-up as scheduled"
} }, {}))
