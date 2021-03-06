#!/bin/sh

if [ $# -ne 2 ] 
then
    echo "Usage: testclient.sh <username> <password>"
    exit
fi 

LD_LIBRARY_PATH=/opt/moonshot/lib64 SAML_EC_IDP='https://boingo.ncsa.uiuc.edu/idp/profile/SAML2/SOAP/ECP' gss-sample/gss-client -nw -nx -nm -port 3490 -user "$1" -pass "$2" -mech "{ 1 3 6 1 4 1 11591 4 6 }" localhost test testmessage
