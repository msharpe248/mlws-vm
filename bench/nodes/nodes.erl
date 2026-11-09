%% The node benchmark (client.masm) for Erlang/OTP's distribution, for
%% comparison: two nodes on this machine, the second halted at the end.
%%     erlc nodes.erl
%%     erl -noshell -sname n2 -setcookie bench -eval 'nodes:srv()' &
%%     erl -noshell -sname n1 -setcookie bench -eval "nodes:cli(['n2@$(hostname -s)'])"
-module(nodes).
-export([srv/0, cli/1]).
sink(N) -> receive {count, From} -> From ! {count, N}, sink(0); _ -> sink(N + 1) end.
echo() -> receive {call, From, V} -> From ! {reply, V}, echo() end.
srv() ->
    register(sink, spawn(fun() -> sink(0) end)),
    register(echo, spawn(fun echo/0)),
    receive stop -> ok end.
t(What, N, F) ->
    T0 = erlang:monotonic_time(nanosecond), F(), T = erlang:monotonic_time(nanosecond) - T0,
    io:format("~s: ~p ns each, ~p/s~n", [What, T div N, N * 1000000000 div T]).
calls(Node, V, N) -> [begin {echo, Node} ! {call, self(), V}, receive {reply, _} -> ok end end || _ <- lists:seq(1, N)], ok.
sends(Node, V, N) ->
    [{sink, Node} ! V || _ <- lists:seq(1, N)],
    {sink, Node} ! {count, self()},
    receive {count, N} -> ok; {count, M} -> io:format("LOST ~p~n", [M]) end.
cli([Node]) ->
    true = net_kernel:connect_node(Node),
    timer:sleep(200),
    L = lists:seq(0, 99),
    M = maps:from_list([{"key" ++ integer_to_list(I), I} || I <- lists:seq(0, 19)]),
    C = lists:foldl(fun(I, T) -> [I | T] end, [], lists:seq(0, 99)),
    t("call [1]", 20000, fun() -> calls(Node, [1], 20000) end),
    t("send [1]", 300000, fun() -> sends(Node, [1], 300000) end),
    t("call 100 ints", 20000, fun() -> calls(Node, L, 20000) end),
    t("send 100 ints", 100000, fun() -> sends(Node, L, 100000) end),
    t("send map of 20", 100000, fun() -> sends(Node, M, 100000) end),
    t("send 100 cells", 50000, fun() -> sends(Node, C, 50000) end),
    rpc:cast(Node, erlang, halt, []),
    timer:sleep(100),
    halt().
