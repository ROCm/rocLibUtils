#!/usr/bin/env zsh

initial_dir=$(pwd)
workdir=$(mktemp -d)
if [[ ! "$workdir" || ! -d "$workdir" ]]; then
    echo "Could not create temporary directory." >&2
    exit 1
fi

function cleanup() {
    cd "${initial_dir}"
    rm -rf "${workdir}"
}

trap cleanup EXIT
cd ${workdir}

# Zsh >= 5.8
zmodload zsh/zutil
zparseopts -D -F - \
    h=help -help=help \
    u:=user -user:=user \
    t:=target -target:=target \
    b:=branch -branch:=branch \
    v+=verbosity -verbose+=verbosity \
    r+:=reviewers -reviewer+:=reviewers \
    -target-org:=target_org \
    -public-only=public \
    -internal-only=internal \
    -include-special=special \
    -commit-message:=commit_message \
    -pr-title:=pr_title \
    -pr-body:=pr_body \
    -dry-run=dry_run \
    || exit 1
autoload throw catch

if (( ${#help} )); then
    cat <<EOF
Usage: $0 [OPTION...] COMMAND [ARGS]
Run some command on each of the ROCm libraries and create a pull request.
The default behaviour is to run on the internal repositories when they exist,
and on the public repositories otherwise.

Options:
    -h, --help              Print this message and exit.
    -u, --user <user>       The GitHub user to push the changes to.
    -t, --target <target>   The target branch to create a PR to (default: develop).
    -b, --branch <branch>   The name of the branch to create (default: bulk-edit).
    -v, --verbose           Increase the verbosity of the process.
    --target-org <org>      The name of the organization to create a PR into (default: ROCmSoftwarePlatform).
    --public-only           Run the command only on the public repositories.
    --internal-only         Run the command only on the internal repositories.
    --include-special       Run the command on the special repositories as well.
    --commit-message <msg>  The commit message to use for the change.
    --pr-title <title>      The title to use for the PR.
    --pr-body <body>        The body to use for the PR.
    --dry-run               Do not push any commits or create any PRs.
EOF
    exit 0
fi

internal_repos=( {roc{BLAS,SPARSE,FFT,ALUTION},hipFFT}-internal )
public_repos=( {roc,hip}{BLAS,SPARSE,FFT,SOLVER,RAND} roc{ALUTION,HPCG,Thrust,PRIM} Tensile hipCUB RCCL )
special_repos=( roc{Jenkins,libs} )

if (( ${#user} == 0 )); then
    user=""
else
    user="${user[2]}/"
fi

used_repos=()
if (( ${#public} > 0 && ${#internal} > 0 )); then
    echo "Only one of --public-only and --internal-only are allowed." >&2
    exit 1
elif (( ${#public} > 0 )); then
    used_repos=( ${public_repos[*]} )
elif (( ${#internal} > 0 )); then
    used_repos=( ${internal_repos[*]} )
else
    for repo in $public_repos; do
        if (( ${internal_repos[(Ie)${repo}-internal]} )); then
            used_repos+=(${repo}-internal)
        else
            used_repos+=(${repo})
        fi
    done
fi
if (( ${#special} > 0 )); then
    used_repos+=( ${special_repos[*]} )
fi

if (( ${#target} > 0 )); then
    target=${target[2]}
else
    target=develop
fi

if (( ${#branch} > 0 )); then
    branch=${branch[2]}
else
    branch=bulk-edit
fi

if (( ${#target_org} > 0 )); then
    target_org=${target_org[2]}
else
    target_org=ROCmSoftwarePlatform
fi

if (( ${#commit_message} > 0 )); then
    commit_message=${commit_message[2]}
else
    vared -p "What should the commit message be? " -c commit_message
fi

if (( ${#pr_title} > 0 )); then
    pr_title=${pr_title[2]}
else
    vared -p "What should the PR title be? " -c pr_title
fi

if (( ${#pr_body} > 0 )); then
    pr_body=${pr_body[2]}
else
    vared -p "What should the PR body be? " -c pr_body
fi

# Starting GitHub stuff
if ! gh auth status &>/dev/null; then
    # Not logged in
    if ! gh auth login; then
        echo "Could not login to GitHub. Exiting." >&2
        exit 1
    fi
fi

vecho() {
    if (( $#verbosity >= $1 )); then
        echo ${@[2,-1]}
    fi
}

unset pr_urls
declare -A pr_urls
failstatus=0
for repo in $used_repos; do
    vecho 1 "Starting ${repo}"
    {
        if (( $(gh pr list -H ${branch} -R "${target_org}/${repo}" | wc -l) > 0 )); then
            vecho 1 "This PR has already been created."
            continue
        fi
        vecho 2 "Starting clone of ${user}${repo}"
        gh repo clone "${user}${repo}" "${repo}" &>/dev/null || throw FailedClone
        vecho 1 "Finished clone of ${user}${repo}"
        cd ${repo}
        git remote | grep upstream &>/dev/null || throw NoFork
        git config --add remote.upstream.fetch "+refs/heads/${target}:refs/remotes/upstream/${target}"
        git fetch upstream &>/dev/null
        vecho 2 "Fetched ${repo}/${target} from upstream."
        git checkout -b ${branch} upstream/${target} --no-track &>/dev/null || throw BranchDNE
        if ! which "${@[1]}" &>/dev/null; then
            # The user's command is not in the path
            if [ -x "${initial_dir}/${@[1]}" ]; then
                # run the user's command relative to the initial directory
                vecho 2 "Running local script ${initial_dir}/${@[1]}"
                command=(${initial_dir}/${@[1]} ${@[2,-1]} $repo)
            else
                throw CommandNotFound
                echo "Could not locate command ${@[1]}." >&2
                exit 1
            fi
        else
            command=($@ $repo)
        fi
        vecho 1 "Running command $command"
        if ! $command; then
            throw CommandFailed
        fi
        cd "${workdir}/${repo}"  # Go back in case user command moved
        git add -A
        git commit -m "${commit_message}"
        vecho 2 "Committed."
        if (( ${#dry_run} > 0 )); then
            git status
            echo "Would create a commit to ${branch} and a PR to ${target_org}/${repo}/${target} with the above changes!"
        else
            git push -uf origin ${branch}
            pr_urls[$repo]=$(gh pr create -B ${target} -R ${target_org}/${repo} --title "${pr_title}" --body "${pr_body}" ${reviewers})
        fi
        vecho 1 "Done repository ${repo}."
    } always {
        if catch FailedClone; then
            failStatus=1
            echo "Failed to clone ${user}${repo}" >&2
        elif catch NoFork; then
            failStatus=1
            echo "${user}${repo} does not appear to be a clone of anything." >&2
        elif catch BranchDNE; then
            failStatus=1
            echo "Branch ${target} does not appear to exist on ${target_org}/${repo}." >&2
        elif catch CommandNotFound; then
            failStatus=1
            echo "${@[1]} could not be found as a command or a local script." >&2
        elif catch CommandFailed; then
            echo "Failed or skipped on ${repo}."
        fi
        cd ${workdir}
        rm -rf ${repo}
    }
    unset CAUGHT
    unset EXCEPTION
done

if (( ${#pr_urls} > 0 )); then
    echo "Created PRs:"
    for repo url in ${(kv)pr_urls}; do
        echo ${repo}: $url
    done
fi
