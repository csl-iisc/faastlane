from botocore.vendored import requests
import json
import boto3
import logging
import threading
import sys

def timeout(event, context):
  raise Exception('Execution is about to time out, exiting...')

def mask_entities_in_message(message, entity_list):
  for entity in entity_list:
      message = message.replace(entity['Text'], '#' * len(entity['Text']))
  return message

def handler(event, context):
  print ('Received message payload')
  try:
      # Extract the entities and message from the event
      message = event['body']['message']
      entity_list = event['body']['entities']
      # Mask entities
      masked_message = mask_entities_in_message(message, entity_list)
      print (masked_message)
      return masked_message
  except Exception as e:
      logging.error('Exception: %s. Unable to extract entities from message' % e)
      raise e

# if __name__=="__main__":
#     print(handler({'body': {'message': 'Pt is 87 yo woman, highschool teacher with past medical history that includes   - status post cardiac catheterization in April 2019.She presents today with palpitations and chest pressure.HPI : Sleeping trouble on present dosage of Clonidine. Severe Rash  on face and leg, slightly itchy  Meds : Vyvanse 50 mgs po at breakfast daily, Clonidine 0.2 mgs -- 1 and 1 / 2 tabs po qhs HEENT : Boggy inferior turbinates, No oropharyngeal lesion Lungs : clear Heart : Regular rhythm Skin :  Mild erythematous eruption to hairline Follow-up as scheduled', 'entities': [{'Id': 0, 'BeginOffset': 6, 'EndOffset': 8, 'Score': 0.9997479319572449, 'Text': '87', 'Category': 'PROTECTED_HEALTH_INFORMATION', 'Type': 'AGE', 'Traits': []}, {'Id': 1, 'BeginOffset': 19, 'EndOffset': 37, 'Score': 0.19382844865322113, 'Text': 'highschool teacher', 'Category': 'PROTECTED_HEALTH_INFORMATION','Type':'PROFESSION', 'Traits': []}, {'Id': 2, 'BeginOffset': 121, 'EndOffset': 131, 'Score': 0.9997519850730896, 'Text': 'April 2019', 'Category': 'PROTECTED_HEALTH_INFORMATION', 'Type':'DATE','Traits': []}]}}
#            ,{}))
