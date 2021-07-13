# Ideally should not do this, but well for testing!
from aws import AWS

platform = AWS(debug=False)
size = 2048
testMs = 50

print(platform.profile(testMs, size, True))
