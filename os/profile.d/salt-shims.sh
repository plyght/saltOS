#!/bin/sh
case ":$PATH:" in
  *:/usr/local/salt/shims:*) ;;
  *) PATH="/usr/local/salt/shims:$PATH" ;;
esac
export PATH
