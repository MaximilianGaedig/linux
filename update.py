import os

def update_kernel_config(config_file, options):
    # Check if the config file exists
    if not os.path.isfile(config_file):
        print(f"Config file {config_file} does not exist.")
        return

    # Read the current config file
    with open(config_file, 'r') as file:
        lines = file.readlines()

    # Prepare to track changes to the options
    option_set = set(options)
    updated_lines = []
    found_options = set()

    for line in lines:
        if any(line.startswith(f"{opt}=") for opt in options):
            # Replace the line if it matches an option we want to update
            for opt in options:
                if line.startswith(f"{opt}="):
                    updated_lines.append(f"{opt}=y\n")
                    found_options.add(opt)
                    break
        else:
            # Otherwise, preserve the original line
            updated_lines.append(line)

    # Add any options that were not found in the file
    for opt in option_set - found_options:
        updated_lines.append(f"{opt}=y\n")

    # Write the updated config back to the file
    with open(config_file, 'w') as file:
        file.writelines(updated_lines)

    print(f"Updated {config_file} with the following options set to 'y':")
    for option in options:
        print(f"  {option}")

if __name__ == "__main__":
    # Path to the kernel .config file
    config_file = '.config'

    # List of options to set to 'y'
    options = [
        "CONFIG_BRIDGE",
        "CONFIG_BRIDGE_NETFILTER",
        "CONFIG_IP6_NF_IPTABLES",
        "CONFIG_IP6_NF_RAW",
        "CONFIG_NETFILTER_XT_MATCH_HASHLIMIT",
        "CONFIG_NETFILTER_XT_MATCH_PHYSDEV",
        "CONFIG_NETFILTER_XT_MATCH_SOCKET",
        "CONFIG_NFT_BRIDGE_META",
        "CONFIG_NFT_BRIDGE_REJECT",
        "CONFIG_NFT_REJECT",
        "CONFIG_NFT_REJECT_IPV4",
        "CONFIG_NFT_REJECT_IPV6",
        "CONFIG_NFT_REJECT_NETDEV",
        "CONFIG_NFT_SOCKET",
        "CONFIG_NFT_TPROXY",
        "CONFIG_NF_TABLES_BRIDGE",
        "CONFIG_NF_TPROXY_IPV6"
    ]

    # Update the kernel config file
    update_kernel_config(config_file, options)

