load("cirrus", "env", "re")

def os_only_list(msg):
    os_only = []
    for line in msg.splitlines():
        if not line.startswith('ci-os-only:'):
            continue
        line = line.lstrip('ci-os-only:')
        os_only += re.findall(r'[^\s\n,]+', line)
    return os_only

def main():
    additional_env = {}

    os_list = os_only_list(env.get('CIRRUS_CHANGE_MESSAGE'))
    additional_env['OS_LIST'] = os_list
    credentials = {
      'workload_identity_provider': env.get('GCP_WORKLOAD_IDENTITY_PROVIDER'),
      'service_account': env.get('GCP_SERVICE_ACCOUNT'),
    }
    return {'env': additional_env, 'gcp_credentials': credentials}
