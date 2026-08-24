# Sequentia Core 24.7.2

Completes the cross-chain half of automatic reward conversion: the states a swap
could get into and never get out of, and the Qt wallet's view of the one stretch
where a conversion is not instantaneous.

There is no consensus change here.

## A swap could say your asset was locked when it was not

A cross-chain swap agrees a timelock before it funds anything. `listrewardswaps`
showed the countdown as soon as the timelock existed, so a swap that agreed
terms and then never funded reported a refund countdown for an asset that had
never left the wallet.

The countdown now appears only once something is actually locked.

## A swap interrupted before funding is no longer stranded

If the node stopped between agreeing terms and funding, the record sat
unfinished for good: the resume pass only ever looked at swaps that had been
funded. Nothing was ever at risk — no money had moved — but an unfinished swap
is something a staker has to keep wondering about.

Such a swap is now written off. It is deliberately **not** funded instead: the
maker's session ended when the node did, and by then it has almost certainly
taken its own Bitcoin back, so funding would lock the staker's asset until our
own timelock for no possible gain. The write-off waits out the longest a live
pass can hold a swap, so the resume pass can never retire one that another
thread is still working on and then have that thread fund against a record
already marked dead.

## Where the asset went

A swap that ends in a refund now records the refund's txid, the same way one
that ends in a claim records the claim, and `listrewardswaps` reports it as
`refund_txid`.

## Qt shows swaps in flight

Converting into Bitcoin is a swap across two chains, so there is a stretch where
the asset is locked in a contract and the Bitcoin has not arrived. The Qt
Staking page now shows what is in that stretch — what is being sold, for how
much, which stage it has reached, and what happens if nobody does anything at
all — with a button to carry them on rather than wait for the wallet's own
two-minute pace. It appears only while there is a swap to show.

Nothing there asks the staker to act. The wallet claims the Bitcoin the moment
the maker reveals the secret and takes the asset back if the maker never does,
watched or not. What the view answers is the question that arises anyway once an
asset has left the balance: where is it, and what happens next.
