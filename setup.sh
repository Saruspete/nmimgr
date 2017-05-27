#!/bin/bash

set -u
export LC_ALL=C

typeset MYSELF="$(readlink -f $0)"
typeset MYPATH="${MYSELF%/*}"

#[[ "$USER" != "root" ]] && {
#	echo "You must run this script as root"
#}
typeset KRN_MODNAME="nmimgr"

typeset BIN_DMIDECODE="$(type -p dmidecode)"
typeset CFG_MODPROBE="/etc/modprobe.d/${KRN_MODNAME}.conf"

# Overridable values from environment
typeset NMI_PANIC="${NMI_PANIC:-}"
typeset NMI_DROP="${NMI_DROP:-}"
typeset NMI_IGNORE="${NMI_IGNORE:-}"
typeset HW_VENDOR="${HW_VENDOR:-}"
typeset HW_MODEL="${HW_MODEL:-}"

typeset ERR_GUESS=""

# Try to run a process with root grants
function runroot {
	if [[ $UID -eq 0 ]] || [[ "${NAME:-}" == "root" ]]; then
		"$@"
	else
		sudo -n -- "$@"
	fi
}

function isModAvail {
	[[ -d "/sys/module/$KRN_MODNAME" ]]
}
function isModPluggable {
	[[ -e "/sys/module/$KRN_MODNAME/refcnt" ]]
}

# Set the option for a module
function optSetMod {
	typeset cfgline="options $KRN_MODNAME"
	[[ -n "$NMI_PANIC" ]] && cfgline="$cfgline events_panic=$NMI_PANIC"
	[[ -n "$NMI_DROP" ]] && cfgline="$cfgline events_drop=$NMI_DROP"
	[[ -n "$NMI_IGNORE" ]] && cfgline="$cfgline events_ignore=$NMI_IGNORE"

	echo "[I] Writing '$cfgline' into file '$CFG_MODPROBE'"
	echo "$cfgline" | runroot tee $CFG_MODPROBE >/dev/null
}

function optSetCmd {

	typeset cfgline=""
	[[ -n "$NMI_PANIC" ]] && cfgline="$cfgline $KRN_MODNAME.events_panic=$NMI_PANIC"
	[[ -n "$NMI_DROP" ]] && cfgline="$cfgline $KRN_MODNAME.events_drop=$NMI_DROP"
	[[ -n "$NMI_IGNORE" ]] && cfgline="$cfgline $KRN_MODNAME.events_ignore=$NMI_IGNORE"

	# Redhat-like
	if [[ -e /etc/sysconfig/grub ]]; then
		echo "[I] Updating configuration from /etc/sysconfig/grub"
		runroot sed '/^GRUB_CMDLINE_LINUX=/s/nmimgr\.[^ ]+//g' -i /etc/sysconfig/grub
		runroot sed '/^GRUB_CMDLINE_LINUX=/s/"$/'$cfgline'"/' -i /etc/sysconfig/grub
		return $?
	else
		echo "[E] Unmanaged boot loader. Please add these parameters to your boot cmdline:"
		echo "    $cfgline"
		return 1
	fi
}

# Try to get the hardware information
if [[ -n "$BIN_DMIDECODE" ]] && [[ -x "$BIN_DMIDECODE" ]]; then
	if [[ -z "$HW_VENDOR" ]]; then
		HW_VENDOR="$(runroot "$BIN_DMIDECODE" -t1 | awk '$1=="Manufacturer:" { $1=""; sub(/^[ \t\r\n]+/, "", $0); print}')"
	fi
	if [[ -z "$HW_MODEL" ]]; then
		HW_MODEL="$(runroot "$BIN_DMIDECODE" -t1 | awk '$1$2=="ProductName:" { $1=$2=""; sub(/^[ \t\r\n]+/, "", $0); print}')"
	fi
fi

# Some hints
echo "[I] Using HW Vendor: '$HW_VENDOR'"
echo "[I] Using HW Model:  '$HW_MODEL'"

#
# Check our values
#
case "$HW_VENDOR" in
	#
	# "Dell Inc."
	#
	Dell*)
		case "$HW_MODEL" in
			# Gen 9...
			PowerEdge\ [12345]9*)		NMI_PANIC="48" ;;
			# Gen10/11/12/13
			PowerEdge\ [RMTC]?[0123]*)	NMI_PANIC="40,41,56,57" ;;
			# Gen14
			PowerEdge\ [RMTC]?4*)		NMI_PANIC="32,48" ;;
			# Unknown
			*)							ERR_GUESS="Unknown Dell model: '$HW_MODEL'" ;;
		esac
	;;

	#
	# "HP", "Hewlett Packard",
	#
	Hewlett\ Packard*)
		case "$HW_MODEL" in
			#*ProLiant*[BMD]L*G7)		NMI_PANIC="32,48" ;;
			*ProLiant*[BMD]L*Gen[89])	NMI_PANIC="39,43" ;;
			*)							ERR_GUESS="Unknown HP model: '$HW_MODEL'" ;;
		esac
	;;

	#
	# IBM
	#
	IBM*)
		case "$HW_MODEL" in
			*iDataPlex*)	NMI_PANIC="44,60" ;;
			*)				ERR_GUESS="Unknown IBM model: '$HW_MODEL'" ;;
		esac
	;;

	#
	# Asus
	#
	ASUSTeK*)
		case "$HW_MODEL" in
			*)				ERR_GUESS="Unknown Asus model: '$HW_MODEL'" ;;
		esac
	;;


	#
	# All other non-manufacturer specific stuff
	#
	*)
		case "$HW_MODEL" in
			VirtualBox)		NMI_PANIC="0,16,32,48" ;;
			*)				ERR_GUESS="Unknown manufacturer and model: '$HW_VENDOR' / '$HW_MODEL'" ;;
		esac
	;;
esac


# Unable to guess the model
[[ -n "$ERR_GUESS" ]] && {
	echo "I cannot guess the configuration for this hardware"
	echo "$ERR_GUESS"
	echo
	echo "Please report this issue on https://github.com/saruspete/nmimgr/issues"
	exit 1
}

echo "[I] Guessed NMI Events:"
echo "    Panic:  $NMI_PANIC"
echo "    Drop:   $NMI_DROP"
echo "    Ignore: $NMI_IGNORE"
echo


#
# Apply the guessed NMI Codes in modprobe or cmdline
#
echo -n "[I] Current implementation: "
if ! isModAvail || isModPluggable; then
	echo "module ($KRN_MODNAME)"

	echo "[I] Updating files"
	optSetMod

elif isModAvail && ! isModPluggable; then
	echo "built-in"

	echo "[I] Updating files"
	optSetCmd

fi


#
# Build the module if present here
#
if [[ -f "$MYPATH/Makefile" ]] && [[ -f "$MYPATH/$KRN_MODNAME.c" ]]; then

	# Try to create the module for each kernel version
	for kpath in /lib/modules/*; do
		typeset kvers="${kpath##*/}"

		! [[ -d "$kpath/build/include" ]] && {
			echo "[W] Skipping $kvers (cannot find '$kpath/build/include')"
			continue
		}

		echo "[I] Building module for kernel $kvers..."
		typeset buildout="$(runroot bash -c "cd "$MYPATH"; make $kvers" 2>&1)"

		typeset kmod="$MYPATH/$KRN_MODNAME.kmod.$kvers/$KRN_MODNAME.ko"
		typeset kdst="/lib/modules/$kvers/extra/nmi"
		if [[ -e "$kmod" ]]; then

			echo "[I] Copying '$kmod' to '$kdst'"
			[[ -d "$kdst" ]] || runroot mkdir -p "$kdst"
			runroot cp "$kmod" "$kdst/"

			# Refresh mods
			depmod -a $kvers
		else
			echo "[E] Kmod build failed. Cannot find '$kmod'"
			echo "[E] Build output begin"
			echo "$buildout"
			echo "[E] Build output end"
		fi
	done

else
	echo "[W] Cannot build + install module: missing Makefile & $KRN_MODNAME.c"
fi

