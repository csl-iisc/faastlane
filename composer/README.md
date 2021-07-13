# Supported Platforms  
usage: python3 generator.py [-h] --input INPUT [--platform {aws,ow}] [--mpk MPK]

optional arguments:
  -h, --help           show this help message and exit
  --input INPUT        Input dir containing fns and workflow
  --platform {aws,ow}  platform to deploy
  --mpk MPK            Use MPK for thread protection

The INPUT directory can optionally contain faastlane.json which specifies custom setting for function deployment on platform 

## AWS
Faastlane uses boto3 client to deploy functions on AWS.

## Apache Openwhisk
Faastlane uses wsk CLI to deploy functions on Openwhisk. Users can create faastlane.json in their directory to pass all arguments they would otherwise pass using wsk CLI.
